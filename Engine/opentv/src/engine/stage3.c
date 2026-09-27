/*
 * Stage 3: phonetics.
 *
 * Stage 3 is where the phoneme string finally becomes sound: it walks the
 * nodes stage 2 sized and writes the 22 synthesis parameter tracks, frame by
 * frame, using the per-phoneme target tables and the transition rules that
 * say how each parameter travels from one phoneme to the next.
 */
#include "engine.h"

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

/* The twenty-four vowels, in the order the stage-3 tables index them. */
/* @0x100bf7d8 */ extern const uint8_t g_vowel_chars[24];

/* Where in that order this phoneme sits, or -1 if it is not one of them. */
/* @0x100386d0 */
int32_t TV_STDCALL Vowel_Index(uint8_t c)
{
    int32_t i;

    for (i = 0; i < 0x18; i++)
        if (g_vowel_chars[i] == c)
            break;
    return (i == 0x18) ? -1 : i;
}

/* One of the four phonemes around stage 3's cursor, chosen by bits 1-2. */
/* @0x10051b70 */
uint8_t TV_THISCALL Stage3_Char(Engine *self, int32_t which)
{
    StageCtx *st = &self->stage_ctx[3];

    switch ((which & 6) >> 1) {
    case 0:  return st->ctl->value;
    case 1:  return st->scan->value;
    case 2:  return st->scan->next->value;
    default: return st->cur->prev->value;
    }
}

/* Flag bits per parameter index (shared with stage 0). */
/* @0x100f83a0 */ extern const uint32_t g_s0_flag_lo[16];

/* The sign-extended index Phone_Attr wants for a phoneme byte. */
static int32_t px3(uint8_t b)
{
    return (int32_t)(int8_t)b;
}

/* @0x1002cdf0 */
int32_t TV_THISCALL Stage3_Pause(Engine *self);
/* @0x10051bd0 */
void TV_THISCALL Stage3_Rules(Engine *self);

/* @0x1002e060 */
Node *TV_THISCALL Stage3_Next(Engine *self);

/* The rest of the stage, not yet decompiled. */
/* @0x1002da60 */
void TV_THISCALL Stage3_Phone(Engine *self);
/* @0x1002dfa0 */
void TV_THISCALL Stage3_Emit(Engine *self);

/*
 * One step of stage 3.
 *
 * The stage keeps three cursors: cur is the phoneme it has finished, ctl the
 * one it is writing, and scan the one after.  Each step moves them on by one
 * phoneme and writes that phoneme's frames into the parameter tracks.
 * s3_1fe0 is how long a transition is allowed, which is longer when the
 * voice is speaking slowly; s3_1fb4 is a small state machine that says how
 * far into the utterance the stage is.
 *
 * Returns 0 idle, 1 when the track buffers are full, 2 when there is more.
 */
/* @0x1002c980 */
int32_t TV_THISCALL Stage3_Run(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *n, *p;
    int32_t freed = 0;
    int32_t v, i;
    uint32_t t;

    Engine_StageBegin(self, st);
    if (st->cur == NULL && st->ctl == NULL && st->scan == NULL)
        goto done;

    if (!(((uint32_t)st->p_34 >> 8) & 0x20) && st->rate_index < 0x13) {
        self->s3_1fe8 = 0x1e;
        self->s3_1fe4 = 0xf;
        self->s3_1fe0 = 0xc;
    } else {
        self->s3_1fe8 = 4;
        self->s3_1fe4 = 3;
        self->s3_1fe0 = 2;
    }

    if (self->trk_08 != -1 &&
        self->trk_0c - self->trk_04 - self->s3_1fe0 <= 8 &&
        self->s3_1fb4 == 0)
        self->s3_1fb4 = 1;

    n = st->ctl;
    v = self->s3_1fb0;
    if (v == 2 && NODE_TYPE(n) == 4 && n->value == ' ' &&
        (n->flags & 0x18u) == 0x18) {
        self->s3_1fbc = 1;
        goto done;
    }

    if (v != 3) {
        if (n == NULL)
            goto advance;
        if (NODE_TYPE(n) == 4)
            goto sized;
    }
    self->s3_1fb0 = 3;
    if (n == NULL)
        goto advance;
    if (NODE_TYPE(n) == 4 &&
        !(n->value == ' ' && (n->flags & 0x18u) == 0x18))
        goto sized;

    /* a control node, or the silence that ends the utterance */
    freed = Stage3_Pause(self);
    Tracks_Op(self, 2, 0);
    self->s3_1fdd = 1;
    self->trk_38 = (freed < 1) ? freed : 1;
    if (freed == 0) {
        n = st->ctl;
        st->cur = n;
        n = Engine_StageNext(self, n);
        st->scan = n;
        st->ctl = n;
        self->s3_1fb0 = 0;
    }
    self->s3_1fb4 = 5;
    return Engine_StageEnd(self) ? 2 : 1;

sized:
    /* a sized phoneme: ask the tracks for room for its frames */
    self->trk_38 = (int32_t)n->arg;
    if (!Tracks_Op(self, 1, (int32_t)n->arg)) {
        self->trk_08 = -1;
        self->synth_busy = 0;
        if (self->s3_1fb4 == 4)
            self->s3_1fb4 = 5;
        return Engine_StageEnd(self) ? 2 : 1;
    }
    if (self->trk_0c - self->trk_04 - self->s3_1fe0 > 0x40) {
        self->trk_08 = -1;
        self->s2_1d54 = 0;
        self->synth_busy = 0;
        if (self->s3_1fb4 == 4)
            self->s3_1fb4 = 5;
    }
    if (st->p_20 == 0) {
        self->trk_08 = -1;
        self->synth_busy = 0;
        if (self->s3_1fb4 == 4)
            self->s3_1fb4 = 5;
    }

advance:
    n = Stage3_Next(self);
    if (n == NULL)
        goto done;
    st->scan = n;
    /* remember the phoneme after the one the scan is on */
    for (;;) {
        n = Engine_StageNext(self, n);
        if (n == NULL)
            break;
        t = NODE_TYPE(n);
        if (t == 4) {
            self->s3_1fec = n->value;
            break;
        }
        if (t != 3) {
            self->s3_1fec = 0x20;
            break;
        }
    }

    if (st->ctl->value != ' ') {
        v = self->s3_1fb4;
        if (v == 0)
            self->s3_1fb4 = 4;
        else if (v == 1)
            self->s3_1fb4 = 2;
    }

    Stage3_Phone(self);
    Stage3_Emit(self);

    n = st->ctl;
    if (!(Phone_Attr(px3(n->value)) & 4))
        self->s3_param_def[17] = self->s3_1fb8;
    if (n->b1a == 1) {
        self->s3_1fb8 = 0x64;
        self->s3_param_def[17] = 0x64;
    }

    p = st->cur;
    if (NODE_TYPE(p) == 4 && p->value == ' ' && (p->flags & 0x18u) == 8) {
        freed = 1;
        Engine_NodeFree(self, p, 1);
    }

    if (self->s3_1fb4 == 2 &&
        self->trk_0c - self->trk_04 - self->s3_1fe0 > 8) {
        self->trk_08 = -1;
        self->s3_1fb4 = 3;
        self->synth_busy = 0;
    }

    self->s3_1fed = st->cur->value;
    st->cur = st->ctl;
    st->ctl = Engine_StageNext(self, st->ctl);
    st->scan = Engine_StageNext(self, st->scan);

    n = st->ctl;
    if (NODE_TYPE(n) == 4 && n->value == ' ' &&
        (n->flags & 0x18u) == 0x18) {
        /* the closing silence: stop the synthesizer where it ends */
        self->synth_busy = 0;
        self->trk_08 = self->trk_0c - self->s3_1fe0;
        p = n;
        for (;;) {
            p = Engine_StageNext(self, p);
            if (p == NULL)
                break;
            t = NODE_TYPE(p);
            if (t == 4 || t == 5)
                break;
        }
        if (p == NULL) {
            Engine_NodeFree(self, st->ctl, 0);
            st->cur = NULL;
            st->ctl = NULL;
            st->scan = NULL;
            i = self->trk_0c - 1;
            self->trk_14[(uint32_t)(i & 0xf8) >> 3] &=
                (uint8_t)~(uint8_t)g_s0_flag_lo[i & 7];
            i = self->trk_0c - self->s3_1fe0 - 1;
            self->trk_14[(uint32_t)(i & 0xf8) >> 3] |=
                (uint8_t)g_s0_flag_lo[i & 7];
        }
    }
    Tracks_Op(self, 2, 0);

done:
    if (!Engine_StageEnd(self) && freed == 0)
        return 0;
    return 2;
}

/*
 * Commit the frames this step wrote.
 *
 * Every parameter advances by its own amount (s3_param[i].len), so after a
 * phoneme the 22 write positions are no longer level.  This finds the
 * earliest and latest of them, marks the earliest frame as ready in trk_14,
 * remembers each parameter's final value for the next phoneme to start from,
 * and lets the read positions catch up to within one transition.
 */
/* @0x1002dfa0 */
void TV_THISCALL Stage3_Emit(Engine *self)
{
    int32_t lo = 0x1000, hi = -1;
    int32_t i, pos, v;

    for (i = 0; i < 22; i++) {
        pos = self->trk_wr[i] + self->s3_param[i].len;
        self->trk_wr[i] = pos;
        self->s3_param_def[i] =
            Synth_ScaleParam(i, self->trk_buf[i][(pos - 1) & 0xff]);
        if (lo > pos)
            lo = pos;
        if (hi < pos)
            hi = pos;
        v = pos - self->s3_1fe0;
        if (self->trk_rd[i] < v)
            self->trk_rd[i] = v;
    }
    i = lo - 1;
    self->trk_14[(uint32_t)(i & 0xf8) >> 3] |= (uint8_t)g_s0_flag_lo[i & 7];
    self->trk_10 = hi;
    self->trk_0c = lo;
}

/* The 22 parameter values a held frame uses, one row per node class. */
/* @0x100f8450 */ extern const uint8_t g_hold_frame[];
/* The row the "s" hold uses. */
/* @0x100f8438 */ extern const uint8_t g_hold_silence[22];
/* How much its own pitch takes off a held frame's amplitude. */
/* @0x100f8548 */ extern const uint8_t g_hold_atten[16];

/*
 * Write the frames of a held sound.
 *
 * A pause, a breath or a held tone is one parameter frame repeated, so this
 * builds that frame once (s3_hold) and then copies it into all 22 tracks for
 * as many frames as there is room for -- at most 53 in one step, so the
 * engine keeps returning to its caller.  What is left to write comes back.
 */
/* @0x1002ced0 */
int32_t TV_THISCALL Stage3_Hold(Engine *self, int32_t ch, int32_t n,
                                int32_t room, int32_t restart)
{
    StageCtx *st = &self->stage_ctx[3];
    const uint8_t *row;
    Node *node;
    int32_t i, k, v, pos, budget;
    /* The original leaves this uninitialised; the row that reads it always
     * writes it first. */
    uint8_t strong = 0;

    if (restart != 0) {
        self->s3_hold_pitch = 0;
        if ((uint8_t)ch == 'g') {
            /* hold whatever the parameters were left at */
            for (i = 0; i < 22; i++)
                self->s3_hold[i] = self->s3_param_raw[i];
        } else if ((uint8_t)ch == 's') {
            for (i = 0; i < 22; i++)
                self->s3_hold[i] = g_hold_silence[i];
        } else if ((uint8_t)ch == 't') {
            node = st->ctl;
            k = (int32_t)((node->flags & 0x18u) >> 3);
            if (node->flags & 0x20u)
                k += 4;
            if (node->flags & 0x40u)
                k += 8;
            row = g_hold_frame + k * 22;
            if (row[9] == 0xff)
                self->s3_hold_pitch = (int32_t)node->b15;
            for (i = 0; i < 22; i++) {
                v = (int32_t)row[i];
                if (row[9] == 0xff && i >= 0xd && i <= 0xf && strong != 0)
                    v = 0x2d;
                if (v == 0xff) {
                    if (i >= 9 && i <= 0xb) {
                        v = self->s3_hold_pitch;
                        strong = (uint8_t)(v > 0xb4);
                    } else if (i == 0x11) {
                        v = (int32_t)st->ctl->b15;
                    } else if (i == 0 || i == 1) {
                        v = 0x3c - st->volume_atten;
                        if (self->s3_hold_pitch != 0)
                            v -= (int32_t)
                                g_hold_atten[self->s3_hold_pitch >> 4];
                        if (v <= 0)
                            v = 1;
                    }
                }
                self->s3_hold[i] = (uint8_t)v;
            }
        }
    }

    pos = self->trk_0c;
    budget = 0x35;
    while (n > 0 && room > 0 && budget != 0) {
        for (i = 0; i < 22; i++)
            self->trk_buf[i][pos & 0xff] = self->s3_hold[i];
        if (self->s3_hold_pitch != 0) {
            k = pos - 1;
            self->s3_1fbd[(uint32_t)(k & 0xf8) >> 3] |=
                (uint8_t)g_s0_flag_lo[k & 7];
        }
        n--;
        pos++;
        room--;
        budget--;
    }
    for (i = 0; i < 22; i++) {
        self->trk_rd[i] = pos - self->s3_1fe0;
        self->trk_wr[i] = pos;
    }
    self->trk_0c = pos;
    self->trk_10 = pos;
    return n;
}

/*
 * The stage-3 step for a held sound.
 *
 * Stage3_Run hands the held node over to this until it has written all of
 * its frames; the count left is the return value, which is what makes the
 * driver come back to the same node next step.
 */
/* @0x1002cdf0 */
int32_t TV_THISCALL Stage3_Pause(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *n;
    int32_t room;

    if (self->s3_1fae != 0) {
        /* a new one: take its length off the node and start the tracks over */
        n = st->ctl;
        if (NODE_TYPE(n) == 4)
            st->ctl = Engine_NodeFree(self, n, 1);
        n = st->ctl;
        self->s3_hold_ch = n->value;
        self->s3_hold_left = (int32_t)(n->arg & 0xffu);
        Stage3_ResetParams(self);
        self->s3_1fb0 = 3;
        self->s3_1fae = 0;
        self->s3_hold_start = 1;
    }

    room = self->trk_04 - self->s3_1fe4 - self->trk_10 + 0x100;
    if (self->s3_hold_left != 0 && room != 0) {
        self->s3_hold_left = Stage3_Hold(self, self->s3_hold_ch,
                                         self->s3_hold_left, room,
                                         self->s3_hold_start);
        if (self->s3_hold_start != 0) {
            self->trk_08 = -1;
            self->synth_busy = 0;
        }
        self->s3_hold_start = 0;
    }
    if (self->s3_hold_left == 0)
        self->s3_1fae = 1;
    return self->s3_hold_left;
}

/*
 * Put a silence in front of the cursor.
 *
 * Stage 3 always needs a phoneme to work on.  When what comes next is a
 * clause break, a stop mark, or simply too many control nodes in a row, it
 * makes one: a pair of silence nodes, the first the run into the silence and
 * the second the silence proper.
 */
/* @0x1002de10 */
Node *TV_THISCALL Stage3_Insert(Engine *self, Node *ref, int32_t mode)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *a, *b;
    uint32_t f;

    a = Engine_NodeAlloc(self, ref, 1, 4, ' ');
    if (mode == 1)
        a->arg = 4;
    else
        a->arg = (uint32_t)(self->s3_1fe0 + self->s3_1fe4 + 0xa);
    a->b15 = (st->ctl != NULL) ? st->ctl->b15 : 0x32;
    f = a->flags & ~0x20u;
    a->flags = f;
    f &= ~0x40u;
    a->flags = f;
    f = (f & ~8u) | 0x10u;
    a->flags = f;

    b = Engine_NodeAlloc(self, a, 1, 4, ' ');
    b->arg = 4;
    b->b15 = (st->ctl != NULL) ? st->ctl->b15 : 0x32;
    f = b->flags & ~0x20u;
    b->flags = f;
    f &= ~0x40u;
    b->flags = f;
    f |= 0x18u;
    b->flags = f;

    if (st->ctl == NULL) {
        st->ctl = a;
        a = Engine_StageNext(self, a);
    }
    self->synth_busy = 0;
    self->trk_08 = -1;
    return a;
}

/*
 * Find the phoneme stage 3 works on next.
 *
 * It looks forward past the control nodes, noting what it passes: a "C"
 * clause command, an "x" stop mark, an end-of-list marker or a run of ten
 * control nodes all mean the sound has to stop, and s3_1fb0 says which.  In
 * those cases it inserts a silence to stop on and returns that instead.
 */
/* @0x1002e060 */
Node *TV_THISCALL Stage3_Next(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *n, *ctl;
    uint32_t f, t;
    int32_t seen = 0, v;
    uint8_t c;

    ctl = st->ctl;
    if (ctl != NULL &&
        (st->cur == ctl || NODE_TYPE(st->cur) != 4)) {
        /* nothing finished yet: give the stage something to start from */
        n = Engine_NodeAlloc(self, ctl, 0, 4, ' ');
        st->cur = n;
        n->arg = 4;
        n->b15 = st->ctl->b15;
        f = n->flags & ~0x20u;
        n->flags = f;
        f &= ~0x40u;
        n->flags = f;
        f = (f & ~0x10u) | 8u;
        n->flags = f;
    }

    n = st->ctl;
    if (n != NULL) {
        for (;;) {
            n = Engine_StageNext(self, n);
            if (n == NULL)
                break;
            t = NODE_TYPE(n);
            if (t == 4) {
                if (self->s3_1fb0 == 3 && n->value == ' ' &&
                    (n->flags & 0x18u) == 0x18)
                    return n;
                self->s3_1fb0 = 0;
                break;
            }
            seen++;
            if (t == 0) {
                c = n->value;
                if (c == 'C') {
                    self->s3_1fb0 = 1;
                    break;
                }
                if (c == 'x' || (self->stop_mark != 0 && c == 'i')) {
                    n->flags |= 0x20u;
                    self->s3_1fb0 = 2;
                    break;
                }
            } else if (t == 5) {
                self->s3_1fb0 = 3;
                break;
            } else if (t == 3 &&
                       (Phone_Attr(px3(n->value) | 0x200) & 8) &&
                       n->value != ']') {
                /* a boundary the synthesizer has to stop on */
                self->trk_08 = -1;
                self->synth_busy = 0;
                if (self->s3_1fb4 == 4)
                    self->s3_1fb4 = 5;
            }
        }
    }

    if (seen >= 0xa)
        self->s3_1fb0 = 4;

    v = self->s3_1fb0;
    if (v != 0 && (n != NULL || v == 4)) {
        if (v == 1)
            return Stage3_Insert(self, n, 1);
        if (v == 4)
            return Stage3_Insert(self, st->last, 4);
        if (v == 2)
            return Stage3_Insert(self, n, 2);
        return n;
    }
    self->trk_08 = -1;
    return n;
}

/* Where in the phoneme's own class table its transition rules live. */
/* @0x100eece0 */ extern const tv_ref g_phone_class;
/* @0x100ef338 */ extern const uint8_t g_class_kind[];
/* How fast the nasal parameters settle, and how far, per voice. */
/* @0x100b52e8 */ extern const int32_t g_voice_nasal_rate[];
/* @0x100b53f0 */ extern const int32_t g_voice_nasal_max[];

/*
 * The starting point for a phoneme's parameters.
 *
 * Most of a phoneme's targets carry over from the one before it; these are
 * the few that do not, which depend on whether the phoneme just finished was
 * voiced, nasal or a stop.
 */
/* @0x10048080 */
void TV_THISCALL Stage3_Defaults(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t ch = px3(st->cur->value);
    uint8_t a = Phone_Attr(ch);

    self->s3_param[17].mode = 6;
    self->s3_param_rate[17] = 0;
    if (!(a & 4)) {
        self->s3_param[0].mode = 6;
        self->trk_rd[17] = self->trk_wr[17];
    }
    if (Phone_Attr(ch | 0x180) & 2) {
        self->s3_param_rate[11] = 0x7ffe;
        self->s3_param_rate[10] = 0x7ffe;
        self->s3_param_rate[9] = 0x7ffe;
    }
    if (a & 0x20)
        self->trk_rd[1] = self->trk_wr[1];
}

/*
 * Lay one parameter's travel into its track.
 *
 * The phoneme rules leave three values behind for each parameter -- where it
 * should be in the frames before this phoneme (lead), where its own run
 * starts (start) and where it settles (target) -- along with the curves to
 * travel along.  "mode" says which of those apply, and this turns them into
 * calls on the track shapers.  Bit 0 asks for a blend backwards over the
 * frames already written, which is only possible if there are any.
 */
/* @0x100051b0 */
void TV_THISCALL Stage3_Write(Engine *self, int32_t param)
{
    S3Param *p = &self->s3_param[param];
    uint8_t *buf = self->trk_buf[param];
    int32_t pos = self->trk_wr[param];
    int32_t span = pos - self->trk_rd[param];
    int32_t mode = p->mode;
    int32_t shape_in = p->shape_in;
    int32_t shape_out = p->shape_out;
    int32_t lead = p->lead;
    int32_t start = p->start;
    int32_t target = p->target;

    if (lead < 0)
        lead = 0;
    if (start < 0)
        start = 0;

    /* the formants and the amplitudes are held wider than a byte */
    switch (param) {
    case 9:
        lead >>= 2;
        start >>= 2;
        target >>= 2;
        break;
    case 10:
        lead -= 0x1f4;
        start -= 0x1f4;
        target -= 0x1f4;
        lead >>= 3;
        start >>= 3;
        target >>= 3;
        break;
    case 11:
    case 12:
        lead >>= 4;
        start >>= 4;
        target >>= 4;
        break;
    case 13:
    case 14:
    case 15:
    case 17:
        lead >>= 1;
        start >>= 1;
        target >>= 1;
        break;
    case 16:
        lead -= 0xc0;
        start -= 0xc0;
        target -= 0xc0;
        lead >>= 2;
        start >>= 2;
        target >>= 2;
        break;
    default:
        break;
    }

    if ((mode & 1) && span <= 0)
        mode &= ~1;

    switch (mode) {
    case 1:
        Track_BlendBack(self, buf, pos, shape_in, span, (uint8_t)lead);
        break;
    case 2:
        Track_BlendFwd(self, buf, pos, shape_out, p->len, (uint8_t)start);
        break;
    case 3:
        Track_BlendBack(self, buf, pos, shape_in, span, (uint8_t)lead);
        Track_BlendFwd(self, buf, pos, shape_out, p->len, (uint8_t)start);
        break;
    case 4:
        Track_Decay(self, buf, pos, 0, p->len, 0, (uint8_t)target);
        break;
    case 5:
        Track_BlendBack(self, buf, pos, shape_in, span, (uint8_t)lead);
        Track_Decay(self, buf, pos, 0, p->len, 0, (uint8_t)target);
        break;
    case 6:
        Track_Decay(self, buf, pos, shape_out, p->len, (uint8_t)start,
                    (uint8_t)target);
        break;
    case 7:
        Track_BlendBack(self, buf, pos, shape_in, span, (uint8_t)lead);
        Track_Decay(self, buf, pos, shape_out, p->len, (uint8_t)start,
                    (uint8_t)target);
        break;
    case 8:
        Track_RampTo(buf, pos, shape_out, (uint8_t)start, (uint8_t)target);
        break;
    case 0x10:
        Track_Line(buf, pos, shape_in, shape_out);
        break;
    case 0x14:
        Track_Decay(self, buf, pos, shape_out, p->len, (uint8_t)start,
                    (uint8_t)target);
        Track_Line(buf, pos, shape_in, shape_out);
        break;
    default:
        break;
    }
}

/* The per-phoneme rule passes, not yet decompiled. */
/* @0x1002d0d0 */
void TV_THISCALL Stage3_Targets(Engine *self);
/* @0x10031050 */
void TV_THISCALL Sapi_PhoneNotify(Engine *self, int32_t ch);

/*
 * Set the parameter targets for one phoneme.
 *
 * This is the middle of stage 3.  It loads the phoneme's own targets, works
 * out which kind of sound it is (s3_flag), runs the context rules that bend
 * those targets toward the phonemes either side, takes the volume off the
 * amplitudes, adds the voice's nasal colouring, and finally asks
 * Stage3_Write to lay each of the 22 parameters into its track.
 */
/* @0x1002da60 */
void TV_THISCALL Stage3_Phone(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *n, *ctl;
    int32_t i, v, atten, voice;
    uint8_t c, d;

    c = st->ctl->value;
    Stage3_Targets(self);
    Stage3_Defaults(self);

    for (i = 0; i < 11; i++)
        self->s3_flag[i] = 0;
    n = st->scan;
    if (n->flags & 0x20u)
        self->s3_flag[10] = 1;
    if (Phone_Attr(px3(c)) & 4)
        self->s3_flag[0] = 1;

    v = self->s3_1e24;
    if (v == 0)
        self->s3_flag[1] = 1;
    else if (v == 3)
        self->s3_flag[2] = 1;
    else if (v == 1)
        self->s3_flag[3] = 1;
    else
        self->s3_flag[4] = 1;

    if ((self->s3_flag[2] != 0 &&
         (Phone_Attr(px3(st->cur->value)) & 0x10)) ||
        self->s3_flag[1] != 0)
        self->s3_flag[3] = 1;

    if (self->s3_1e28 != 0) {
        ctl = st->ctl;
        if (Phone_Attr(px3(ctl->value)) & 0x20) {
            d = n->value;
            if ((Phone_Attr(px3(d) | 0x100) & 1) || d == ' ' ||
                (ctl->flags & 0x40u))
                self->s3_flag[5] = 1;
        }
        v = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(n->value)]];
        if (v == 0)
            self->s3_flag[6] = 1;
        else if (v == 1)
            self->s3_flag[7] = 1;
        else if (v == 3)
            self->s3_flag[8] = 1;
        else if (v == 2)
            self->s3_flag[9] = 1;
    }

    Stage3_Rules(self);

    ctl = st->ctl;
    if (!(Phone_Attr(px3(ctl->value)) & 4)) {
        self->s3_param[17].mode &= ~2;
        self->s3_1fb8 = self->s3_param[17].target;
        self->s3_param[17].target = 0;
    }
    if (ctl->b15 == 0) {
        /* no pitch: flatten the three pitch parameters */
        self->s3_param[0].start = 0;
        self->s3_param[0].lead = 0;
        self->s3_param[17].mode &= ~2;
        v = self->s3_param[0].target;
        self->s3_param[0].target = 0;
        self->s3_param[2].target = v + 5;
    }
    n = st->cur;
    if (!(Phone_Attr(px3(n->value)) & 4) || n->b15 == 0)
        self->s3_param[17].mode &= ~1;

    /* take the voice's attenuation off the amplitudes */
    atten = st->volume_atten;
    for (i = 0; i < 3; i++) {
        v = self->s3_param[i].target - atten;
        self->s3_param[i].target = (v < 0) ? 0 : v;
        v = self->s3_param[i].lead - atten;
        self->s3_param[i].lead = (v < 0) ? 0 : v;
        v = self->s3_param[i].start - atten;
        self->s3_param[i].start = (v < 0) ? 0 : v;
    }

    voice = st->voice;
    if (voice != 0) {
        /* the voice's own nasal colouring */
        v = self->s3_param[9].target;
        self->s3_param[9].target = Synth_MulQ15(v, g_voice_nasal_rate[voice]) + v;
        v = self->s3_param[10].target;
        self->s3_param[10].target =
            Synth_MulQ15(v, g_voice_nasal_rate[st->voice]) + v;
        v = self->s3_param[11].target;
        v = Synth_MulQ15(v, g_voice_nasal_rate[st->voice]) + v;
        self->s3_param[11].target = v;
        self->s3_param[12].target +=
            Synth_MulQ15(v, g_voice_nasal_rate[st->voice]);
        if (self->s3_param[12].target > g_voice_nasal_max[st->voice])
            self->s3_param[12].target = g_voice_nasal_max[st->voice];
    }

    self->s3_param[21].target |= st->voice << 4;
    self->s3_param[20].target |= st->volume_atten << 4;

    if (self->s3_1fdd != 0) {
        for (i = 0; i < 22; i++)
            self->trk_rd[i] = self->trk_wr[i];
        self->s3_1fdd = 0;
    }

    if (self->w_2130 != 0) {
        c = st->ctl->value;
        if (c != 'p')
            Sapi_PhoneNotify(self, (int32_t)c);
    }

    for (i = 0; i < 22; i++)
        Stage3_Write(self, i);
}

/* The rules, reached through the classes of the phoneme being written and
 * the one before it. */
/* @0x10095040 */ extern const tv_ref g_rule_sel[];
/* @0x10094870 */ extern const tv_ref g_rule_set[];
/* @0x10094948 */ extern const uint8_t g_rule_count[];
/* Which attribute bit an "is it ..." condition asks about. */
/* @0x100c89e0 */ extern const uint32_t g_attr_mask[128];

/* @0x10051890 */
void TV_THISCALL Stage3_Apply(Engine *self, const tv_ref *edits,
                              int32_t mode);

/* The sixteen routines a rule can ask for. */
/* @0x10048100 */ void TV_THISCALL Stage3_Op1(Engine *self);
/* @0x100481b0 */ void TV_THISCALL Stage3_Op2(Engine *self);
/* @0x10048310 */ void TV_THISCALL Stage3_Op3(Engine *self);
/* @0x10048620 */ void TV_THISCALL Stage3_Op4(Engine *self);
/* @0x10049100 */ void TV_THISCALL Stage3_Op5(Engine *self);
/* @0x10049260 */ void TV_THISCALL Stage3_Op6(Engine *self);
/* @0x10049570 */ void TV_THISCALL Stage3_Op7(Engine *self);
/* @0x1004a0e0 */ void TV_THISCALL Stage3_Op8(Engine *self);
/* @0x1004a200 */ void TV_THISCALL Stage3_Op9(Engine *self);
/* @0x1004fe70 */ void TV_THISCALL Stage3_Op10(Engine *self);
/* @0x1004b5d0 */ void TV_THISCALL Stage3_Op11(Engine *self);
/* @0x1004be40 */ void TV_THISCALL Stage3_Op12(Engine *self);
/* @0x1004bfc0 */ void TV_THISCALL Stage3_Op13(Engine *self);
/* @0x1004e360 */ void TV_THISCALL Stage3_Op14(Engine *self);
/* @0x100055b0 */ void TV_THISCALL Stage3_Op15(Engine *self);
/* @0x10050c30 */ void TV_THISCALL Stage3_Op16(Engine *self);

/*
 * Run the rules for this phoneme against the one before it.
 *
 * Each rule is a little byte-code program: conditions of three kinds -- is
 * one of the s3_flag bits set, does a neighbouring phoneme have a given
 * attribute, is a neighbouring phoneme one of this list -- and then, if they
 * all hold, a block of parameter edits and a list of routines to run.  The
 * first rule that matches wins.
 */
/* @0x10051bd0 */
void TV_THISCALL Stage3_Rules(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    const S3Rule *r;
    const uint8_t *s;
    int32_t i, count, k, opc, want, v, idx;
    uint32_t mask;
    uint8_t b, c, d, found;

    k = (int32_t)TV_REF(uint8_t,
             g_rule_sel[TV_REF(uint8_t, g_phone_class)[px3(st->ctl->value)]])
                 [TV_REF(uint8_t, g_phone_class)[px3(st->cur->value)]];
    r = TV_REF(S3Rule, g_rule_set[k]);
    count = (int32_t)g_rule_count[k];

    for (i = 0; i < count; i++, r++) {
        s = TV_REF(uint8_t, r->cond);
        if (TV_REF_OK(r->cond)) {
            for (;;) {
                b = *s;
                want = b & 1;
                opc = b >> 3;
                if (opc == 0) {
                    s++;
                    if ((int32_t)self->s3_flag[*s] != want)
                        goto next;
                    s++;
                } else if (opc == 1) {
                    c = Stage3_Char(self, (int32_t)b);
                    s++;
                    mask = g_attr_mask[*s & 0x7f];
                    idx = (int32_t)((mask & 0xff00u) >> 1) | px3(c);
                    v = (int32_t)(Phone_Attr(idx) & mask) & 0xff;
                    if ((v == 0) == want)
                        goto next;
                    s++;
                } else if (opc == 2) {
                    c = Stage3_Char(self, (int32_t)b);
                    found = 0;
                    for (;;) {
                        d = s[1];
                        s++;
                        if (d <= 0x18)
                            break;
                        if (px3(c) == (int32_t)d) {
                            if (want == 0)
                                goto next;
                            found = 1;
                            do {
                                s++;
                            } while (*s > 0x18);
                            break;
                        }
                    }
                    if ((int32_t)found != want)
                        goto next;
                }
                if (*s == 0x18)
                    break;
            }
        }

        if (TV_REF_OK(r->edits))
            Stage3_Apply(self, TV_REF(tv_ref, r->edits), 1);

        s = TV_REF(uint8_t, r->ops);
        if (!TV_REF_OK(r->ops) || *s == 0)
            return;
        do {
            switch (*s++) {
            case 1:  Stage3_Op1(self);  break;
            case 2:  Stage3_Op2(self);  break;
            case 3:  Stage3_Op3(self);  break;
            case 4:  Stage3_Op4(self);  break;
            case 5:  Stage3_Op5(self);  break;
            case 6:  Stage3_Op6(self);  break;
            case 7:  Stage3_Op7(self);  break;
            case 8:  Stage3_Op8(self);  break;
            case 9:  Stage3_Op9(self);  break;
            case 10: Stage3_Op10(self); break;
            case 11: Stage3_Op11(self); break;
            case 12: Stage3_Op12(self); break;
            case 13: Stage3_Op13(self); break;
            case 14: Stage3_Op14(self); break;
            case 15: Stage3_Op15(self); break;
            case 16: Stage3_Op16(self); break;
            default: break;
            }
        } while (*s != 0);
        return;
next:
        ;
    }
}

/*
 * Make the parameter edits a rule asks for.
 *
 * The edits come as NULL-terminated blocks of twelve-byte records, each one
 * naming a value (a field of one s3_param, a track position, one of the
 * stage's own variables) and what to do to it.  Records also carry a mask so
 * the same block can serve several passes; "mode" selects which apply.  An
 * accumulator carries a value from one record to the next.
 */
/* @0x10051890 */
void TV_THISCALL Stage3_Apply(Engine *self, const tv_ref *edits,
                              int32_t mode)
{
    const tv_ref *blk = edits + 1;
    tv_ref eref = edits[0];
    const S3Edit *e = TV_REF(S3Edit, eref);
    /* The original seeds both of these from the block cursor; a record that
     * used either before setting it would be writing into the rule table. */
    int32_t *p = (int32_t *)(void *)blk;
    int32_t acc = (int32_t)(intptr_t)blk;
    int32_t i;

    while (TV_REF_OK(eref)) {
        for (;;) {
            if (((int32_t)e->when & mode) != 0) {
                switch (e->field) {
                case 0:  p = &self->s3_param[e->index].target;    break;
                case 1:  p = &self->s3_param[e->index].mode;      break;
                case 2:  p = &self->s3_param[e->index].shape_out; break;
                case 3:  p = &self->s3_param[e->index].shape_in;  break;
                case 4:  p = &self->s3_param[e->index].start;     break;
                case 5:  p = &self->s3_param[e->index].lead;      break;
                case 6:  p = &self->s3_param[e->index].len;       break;
                case 7:  p = &self->trk_wr[e->index];             break;
                case 8:  p = &self->trk_rd[e->index];             break;
                case 9:  p = &self->s3_next[e->index];            break;
                case 10: p = &self->s3_param_def[e->index];       break;
                case 11: p = &self->s3_param_rate[e->index];      break;
                case 12: p = &self->s3_1e5c[e->index];            break;
                case 13: p = &self->s3_1e24;                      break;
                case 14: p = &self->s3_1e28;                      break;
                case 15: p = &self->s3_1e2c;                      break;
                case 16: p = &self->s3_1e30;                      break;
                case 17: p = &self->s3_1e34;                      break;
                case 18: p = &self->s3_1e38;                      break;
                case 19: p = &self->s3_1e3c;                      break;
                case 20: p = &self->s3_1e4c;                      break;
                case 21: p = &self->s3_1e50;                      break;
                case 22: p = &self->s3_1e54;                      break;
                case 23: p = &self->s3_1e58;                      break;
                case 24: p = &self->s3_1e48;                      break;
                case 25: p = &self->s3_1e44;                      break;
                default: break;   /* keeps the one before */
                }
                switch (e->op & 0x7f) {
                case 0: *p = e->arg;                        break;
                case 1: *p += e->arg;                       break;
                case 2: *p -= e->arg;                       break;
                case 3: *p = Synth_MulQ15(*p, e->arg);      break;
                case 4: *p += Synth_MulQ15(*p, e->arg);     break;
                case 5: acc = e->arg;                       break;
                case 6: acc = *p;                           break;
                case 7: *p = acc;                           break;
                case 8:
                    for (i = 0; e->arg >= i; i++)
                        p[i] = acc;
                    break;
                default: break;
                }
            }
            if (!(e->op & 0x80))
                break;
            e++;
        }
        eref = *blk++;
        e = TV_REF(S3Edit, eref);
    }
}

/* @0x1004f710 */
void TV_THISCALL Stage3_Coarticulate(Engine *self);

/*
 * Run all five parameter passes, then tidy up after them.
 *
 * The formants have to stay at least 200 Hz apart or the cascade rings, no
 * transition may be longer than the curves allow, and the first formant's
 * amplitude gets a little back.
 */
/* @0x1004fe70 */
void TV_THISCALL Stage3_Op10(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t i, v;

    Stage3_Op11(self);
    Stage3_Op12(self);
    Stage3_Op13(self);
    Stage3_Op14(self);
    Stage3_Coarticulate(self);

    if (st->ctl->value != 'T' || st->scan->value != '3') {
        for (i = 9; i < 12; i++) {
            v = self->s3_param[i].start + 0xc8;
            if (self->s3_param[i + 1].start < v)
                self->s3_param[i + 1].start = v;
            v = self->s3_param[i].lead + 0xc8;
            if (self->s3_param[i + 1].lead < v)
                self->s3_param[i + 1].lead = v;
        }
    }

    for (i = 0; i < 22; i++) {
        if (self->s3_param[i].shape_out > 0x1e)
            self->s3_param[i].shape_out = 0x1e;
        if (self->s3_param[i].shape_in > 0x14)
            self->s3_param[i].shape_in = 0x14;
    }

    if (self->s3_param[1].target > 0)
        self->s3_param[1].target += 7;
}


/* Which of six rules the phoneme just finished calls for. */
/* @0x1004be00 */ extern const uint8_t g_op11_sel[50];

/*
 * How fast the formants may move across this boundary.
 *
 * Most of what this sets is s3_param_rate -- a ceiling on how far a
 * parameter may travel per frame.  Going into a sound the tongue reaches
 * slowly (a nasal, a liquid) the ceiling comes down; coming out of a stop's
 * release it goes up, because the formants really do jump.
 */
/* @0x1004b5d0 */
void TV_THISCALL Stage3_Op11(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    uint8_t a_ctl, a_cur, a_cur0;
    int32_t i, v, k;

    if ((Phone_Attr(c_ctl | 0x100) & 2) && (Phone_Attr(c_cur) & 0x20) &&
        (Phone_Attr(c_cur | 0x100) & 1) && !Phone_IsVowel((uint8_t)c_ctl))
        self->s3_param[11].start = self->s3_param_def[11];

    if (self->s3_1e24 < self->s3_1e28) {
        /* opening: the mouth is getting more open than it was */
        if (c_cur != ' ') {
            if ((Phone_Attr(c_ctl | 0x180) & 4) &&
                !(Phone_Attr(c_ctl) & 2)) {
                self->s3_param_rate[10] = 0x6680;
                self->s3_param[10].shape_out += 3;
            }
            if (Phone_Attr(c_cur | 0x100) & 1)
                return;
            if (!(Phone_Attr(c_ctl | 0x100) & 1))
                return;
            if (c_ctl == 'q' &&
                (Phone_Attr(px3(cur->value) | 0x100) & 2)) {
                self->s3_param_rate[9] = 0x7ed8;
                v = (int32_t)cur->arg;
                self->s3_param[9].shape_in = v - (int32_t)(cur->arg / 3u);
            } else {
                self->s3_param_rate[9] = 0x4010;
            }
            if (!(Phone_Attr(c_ctl | 0x100) & 0x10) && c_ctl != 'T') {
                self->s3_param_rate[11] = 0x7ffe;
                self->s3_param_rate[10] = 0x7ffe;
            }
            if (st->scan->value == 's' && st->scan->next->value == ' ' &&
                c_cur != ' ') {
                self->s3_param_rate[11] = 0xcd0;
                self->s3_param_rate[10] = 0xcd0;
            }
            if (c_ctl == 'K') {
                self->s3_param_rate[9] = 0x7ffe;
                if (Phone_Attr(px3(cur->value) | 0x180) & 0x10)
                    self->s3_param_rate[10] = 0x19a0;
            }
        }

        if (c_ctl == '~' || c_ctl == 'm' || c_ctl == 'n') {
            for (i = 9; i < 17; i++)
                self->s3_param[i].mode = 5;
            self->s3_param_rate[9] = 0x6018;
            if (Phone_Attr(c_cur) & 2) {
                self->s3_param[13].shape_out = 0xa;
                self->s3_param[16].shape_out = 0xa;
                self->s3_param_rate[16] = 0x4678;
                self->s3_param_rate[11] = 0x7350;
                if (c_ctl == 'm') {
                    self->s3_param_rate[12] = 0x2670;
                    self->s3_param[12].shape_out = 9;
                    self->s3_param[12].mode = 7;
                } else if (c_ctl == '~') {
                    if (Phone_Attr(px3(st->scan->value)) & 2) {
                        self->s3_param[10].target = 0x384;
                        self->s3_param[11].target = 0x880;
                    } else {
                        self->s3_param[10].target =
                            (self->s3_param_def[10] +
                             self->s3_param[10].target) / 2;
                        self->s3_param[10].shape_out = 7;
                        self->s3_param[11].target =
                            (self->s3_param_def[11] +
                             self->s3_param[11].target) / 2;
                        if (Phone_Attr(c_cur | 0x100) & 0x20) {
                            self->s3_param[10].target = 0x4b0;
                            self->s3_param[11].target = 0x9c4;
                        }
                    }
                }
            } else if (c_cur == ' ') {
                self->s3_param_rate[0] = 0x3340;
                self->s3_param[0].shape_out = 6;
            }
        }
        goto tail;
    }

    /* closing */
    if (c_cur == ' ')
        goto narrow;

    if (c_ctl == 'R') {
        if (c_cur == 's' && cur->prev->value == 'C') {
            self->s3_param_rate[11] = 0x7ed8;
            self->s3_param_rate[10] = 0x7ed8;
        } else {
            self->s3_param_rate[11] = 0x6018;
            self->s3_param_rate[10] = 0x6018;
        }
    } else if ((Phone_Attr(c_cur | 0x180) & 4) &&
               !(Phone_Attr(c_cur) & 2) &&
               !Phone_IsVowel((uint8_t)c_ctl)) {
        self->s3_param_rate[10] = 0x19a0;
        self->s3_param[10].shape_out += 3;
    }

    if ((Phone_Attr(c_ctl | 0x180) & 4) && (Phone_Attr(c_cur | 0x80) & 8) &&
        !Phone_IsVowel(cur->value)) {
        int32_t skip = 0;

        if (c_ctl == 's' && c_cur == 'C' &&
            (Phone_Attr(px3(st->scan->value) | 0x100) & 2))
            skip = 1;
        if (!skip && c_ctl == 's' && c_cur == 'C' &&
            st->scan->value == 'R')
            skip = 1;
        if (!skip && c_ctl == 'z' && c_cur == 'J') {
            k = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(st->scan->value)]];
            if (k == 1 || k == 0x62)
                skip = 1;
        }
        if (!skip)
            self->s3_param[11].target += 0x12c;
    }

    a_ctl = Phone_Attr(c_ctl | 0x100);
    if ((a_ctl & 2) && c_cur == 'S')
        self->s3_param[1].mode = 4;

    a_cur0 = Phone_Attr(c_cur);
    if ((a_cur0 & 0x10) && !(Phone_Attr(c_ctl) & 2)) {
        for (i = 10; i < 13; i++)
            self->s3_param[i].shape_out = 5;
        for (i = 13; i < 17; i++)
            self->s3_param[i].shape_out = 2;
    }

    if (a_ctl & 1)
        return;
    a_cur = Phone_Attr(c_cur | 0x100);
    if (!(a_cur & 1))
        return;
    if ((a_cur0 & 0x20) && (a_cur0 & 4) && (a_ctl & 2))
        self->s3_param[1].mode = 5;

    self->s3_param_rate[9] = 0x4010;
    self->s3_param_rate[11] = 0;
    self->s3_param_rate[10] = 0;

    if ((a_cur & 0x10) && !Phone_IsVowel((uint8_t)c_ctl)) {
        if (!(c_cur == 'P' && c_ctl == 'S')) {
            self->s3_param_rate[10] = 0x19a0;
            self->s3_param_rate[11] = 0x59b0;
        }
        if (a_ctl & 0x20) {
            self->s3_param_rate[10] = 0x4010;
            self->s3_param_rate[11] = 0x19a0;
        }
    } else if ((a_cur & 4) && !Phone_IsVowel((uint8_t)c_ctl)) {
        if (!(a_cur0 & 0x10) && c_cur != 'D' && c_cur != 'T') {
            self->s3_param[10].start = 0x640;
            self->s3_param_rate[10] = 0;
        }
        if (c_cur != 'T') {
            self->s3_param[11].start = 0xa3c;
            self->s3_param_rate[11] = 0;
        }
        if ((a_ctl & 8) && c_cur != 'D') {
            self->s3_param[11].start = 0x8fc;
            self->s3_param_rate[11] = 0;
        }
    } else if (a_cur & 0x80) {
        self->s3_param_rate[9] = 0;
    }

narrow:
    if ((Phone_Attr(c_cur) & 0x10) && (Phone_Attr(c_ctl) & 2)) {
        self->s3_param_rate[9] = 0x2008;
        if (!(Phone_Attr(c_ctl | 0x180) & 0x10) &&
            !Phone_IsVowel((uint8_t)c_ctl)) {
            for (i = 9; i < 12; i++)
                self->s3_param[i].mode = 6;
        } else if (!Phone_IsVowel((uint8_t)c_ctl)) {
            for (i = 12; i < 17; i++)
                self->s3_param[i].mode = 6;
            for (i = 9; i < 12; i++)
                self->s3_param[i].mode = 2;
        }
        if (!(c_cur == '~' && c_ctl == 'p' && st->scan->value == ' ')) {
            self->s3_param_rate[16] = 0x6018;
            self->s3_param[16].shape_out = 9;
            self->s3_param_rate[11] = 0xcd0;
        }
    }

tail:
    i = c_cur - 0x4d;
    switch (((uint32_t)i <= 0x31) ? g_op11_sel[i] : 5) {
    case 0:
    case 2:
        self->s3_param_rate[12] = 0x7350;
        if (Phone_Attr(c_ctl | 0x100) & 0x20) {
            if (c_ctl == '4') {
                self->s3_param_rate[12] = 0x4010;
                self->s3_param[12].shape_out = 5;
                self->s3_param[12].mode |= 1;
            }
        } else {
            self->s3_param_rate[10] = 0xcd0;
        }
        break;
    case 1:
    case 3:
        if (!Phone_IsVowel((uint8_t)c_ctl)) {
            self->s3_param[11].start = 0xa28;
            self->s3_param[12].start = 0xd48;
        }
        self->s3_param_rate[12] = 0;
        self->s3_param_rate[11] = 0;
        if ((Phone_Attr(c_ctl | 0x200) & 1) &&
            !(Phone_Attr(c_ctl | 0x100) & 0x20) &&
            !Phone_IsVowel((uint8_t)c_ctl)) {
            self->s3_param[10].start = 0x686;
            self->s3_param_rate[10] = 0;
        }
        if (c_ctl == 'd' && c_cur == 'N') {
            self->s3_param_rate[18] = 0x19a0;
            self->s3_param_rate[19] = 0x19a0;
            self->s3_param[19].shape_out = 4;
            self->s3_param[18].shape_out = 4;
        }
        break;
    case 4:
        if (c_ctl == 'p' && st->scan->value == ' ') {
            for (i = 0; i < 7; i++) {
                self->s3_param[10 + i].target = self->s3_param_def[10 + i];
                self->s3_param[10 + i].mode = 4;
            }
        }
        break;
    default:
        break;
    }

    if ((Phone_Attr(c_ctl) & 1) && (Phone_Attr(c_cur) & 4) &&
        (cur->flags & 0x20u) && !(cur->flags & 0x40u) &&
        (ctl->flags & 0x20u) && !(ctl->flags & 0x40u)) {
        self->s3_param_rate[17] = 0x4010;
        self->s3_param[17].mode = 7;
    }
}

/* @0x10048ef0 */ void TV_THISCALL Stage3_NasalPole(Engine *self, int32_t *out);

/* Where the formants end up once the nasal murmur is over. */
/* @0x100ef280 */ extern const int32_t g_nasal_target[3];
/* How much of the way there the blend goes, by how long the phoneme is. */
/* @0x100ef2d0 */ extern const int32_t g_nasal_blend[16];
/* What fraction of the first formant's run the murmur takes, by class. */
/* @0x100ef0d0 */ extern const int32_t g_nasal_frac[];
/* Which of six rules the phoneme after a nasal calls for. */
/* @0x10048ec8 */ extern const uint8_t g_op4_sel[0x24];

/* Decide whether the vowel just finished is reduced (not yet decompiled). */
/* @0x10038890 */ uint8_t TV_THISCALL Stage3_Reduce(Engine *self, int32_t vowel);

/*
 * The nasals.
 *
 * A nasal is two sounds in one: the murmur, where the mouth is shut and the
 * sound comes down the nose, and the release into whatever follows.  The
 * murmur has formants of its own that barely depend on which nasal it is, so
 * this splits the phoneme's run in two -- the first s3_1e48 frames get the
 * murmur's values, the rest travel to the following phoneme's -- and writes
 * each half with its own call to Stage3_Write.
 */
/* @0x10048620 */
void TV_THISCALL Stage3_Op4(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    int32_t c_scan = px3(st->scan->value);
    int32_t pole[7];
    int32_t i, k, v, len, tgt;
    int32_t sv_rd, sv_wr, sv_len, sv_lead, sv_start, sv_in, sv_out;
    /* the original leaves this slot holding whichever loop count ran last,
     * so the "M" test at the end of the murmur branch never fires */
    int32_t spare = 0;

    self->s3_2034 = 0;
    i = Vowel_Index(ctl->value);
    if (i != -1) {
        if (Phone_Attr(c_cur | 0x100) & 2)
            self->s3_1e38 = 0x7f;
        self->s3_2034 = Stage3_Reduce(self, i);
        if (self->s3_2034 != 0) {
            self->s3_1e38 = 0x7f;
            return;
        }
    }

    if (c_ctl == 'u' && c_scan == 'j') {
        self->s3_param[10].target -= 0x96;
    } else if (Phone_Attr(c_scan | 0x100) & 0x40) {
        if (Phone_Attr(c_ctl | 0x100) & 0x20) {
            self->s3_alt[1] -= 0x12c;
            self->s3_param[10].target -= 0x12c;
        }
    } else if (Phone_Attr(c_cur | 0x100) & 8) {
        self->s3_param[11].target =
            (3 * self->s3_param[11].target + 0x708) / 4;
        self->s3_alt[2] = (3 * self->s3_alt[2] + 0x708) / 4;
        if (Phone_Attr(c_ctl | 0x100) & 0x20) {
            self->s3_param[10].target =
                (3 * self->s3_param[10].target + 0x578) / 4;
            self->s3_alt[1] = (3 * self->s3_alt[1] + 0x578) / 4;
        }
    }

    if ((Phone_Attr(c_scan) & 0x20) && (Phone_Attr(c_scan | 0x100) & 4) &&
        c_ctl == 'U')
        self->s3_alt[1] += 0x190;

    if ((Phone_Attr(c_cur | 0x180) & 8) && (Phone_Attr(c_ctl | 0x180) & 0x20))
        self->s3_param[10].shape_out += 5;

    if (!(Phone_Attr(c_ctl | 0x100) & 8) &&
        !(Phone_Attr(c_ctl | 0x200) & 0x40)) {
        /* ease the first three formants towards the murmur's own */
        v = 10 * (int32_t)ctl->arg;
        if (v > 0xff)
            v = 0xff;
        v = g_nasal_blend[v / 16];
        spare = 3;
        for (i = 0; i < 3; i++) {
            self->s3_param[9 + i].target +=
                Synth_MulQ15(g_nasal_target[i] - self->s3_param[9 + i].target,
                             v);
            self->s3_alt[i] +=
                Synth_MulQ15(g_nasal_target[i] - self->s3_alt[i], v);
        }
    }

    if (Phone_Attr(c_ctl | 0x80) & 0x10) {
        /* the phoneme that just ended was itself a nasal */
        if (c_ctl == 'p') {
            v = Phone_Attr(c_cur);
            if ((v & 4) && (v & 0x20))
                self->s3_param[0].target = 0x36;
            if (c_cur == 'K') {
                self->s3_param[10].target = 0x674;
                self->s3_param[11].target = 0x740;
            }
        }
        if (!(ctl->flags & 0x20u)) {
            self->s3_param[0].target -= 2;
            if (self->s3_param[0].target < 0)
                self->s3_param[0].target = 0;
        }
        self->s3_param[11].target =
            Synth_MulQ15(self->s3_next[11] + self->s3_param_def[11] +
                         self->s3_param[11].target, 0x2a48);
        if (self->s3_2034 != 0) {
            spare = 0xd;
            for (i = 0; i < 4; i++) {
                self->s3_param[9 + i].start = self->s3_param_def[9 + i];
                self->s3_param[9 + i].mode &= 6;
                self->s3_param[9 + i].shape_out = (int32_t)(ctl->arg >> 1);
            }
        }
        if (cur->value == 'M' && Phone_IsVowel(cur->prev->value) &&
            !(ctl->flags & 0x20u) && spare == 9) {
            self->s3_param[spare].shape_in = (int32_t)cur->arg + 1;
            self->s3_param[spare].mode |= 1;
        }
        return;
    }

    if ((Phone_Attr(c_ctl | 0x180) & 0x10) &&
        !(c_ctl == 'u' && c_scan == 'j')) {
        /* lay down the murmur, then the release */
        len = self->s3_param[9].len;
        v = self->s3_1e44;
        self->s3_1e44 = (int32_t)(((uint32_t)(len * v) / ctl->arg) + v) >> 1;
        self->s3_1e48 =
            Synth_MulQ15(len, g_nasal_frac[TV_REF(uint8_t, g_phone_class)[c_ctl]]);
        if (c_cur == 'N')
            Stage3_NasalPole(self, pole);

        for (k = 9; k <= 15; k++) {
            S3Param *p = &self->s3_param[k];
            int32_t *alt = &self->s3_alt[k - 9];
            int32_t *cell = &self->s3_nasal_tgt[k - 9];

            sv_rd = self->trk_rd[k];
            sv_out = p->shape_out;
            sv_in = p->shape_in;
            sv_start = p->start;
            sv_lead = p->lead;
            tgt = p->target;
            sv_len = p->len;
            sv_wr = self->trk_wr[k];
            p->mode = 7;

            if ((Phone_Attr(c_ctl | 0x200) & 2) && !(ctl->flags & 0x20u))
                tgt -= (tgt - *alt) >> 2;

            *cell = (*alt + tgt) / 2;
            if (k == 11 && c_ctl == 'U')
                self->s3_1e48 -= 7;

            if (self->s3_1e48 > 0) {
                if (p->len <= self->s3_1e48) {
                    *cell = *alt;
                    p->mode = 3;
                    self->s3_1e48 = p->len;
                }
                p->start = (c_cur == 'N') ? pole[k - 9] : tgt;
                p->mode = 6;
                if (Phone_IsVowel((uint8_t)c_cur))
                    p->start = self->s3_param_def[k];
                if (cur->value == 'M' && Phone_IsVowel(cur->prev->value) &&
                    !(ctl->flags & 0x20u) && k == 9) {
                    p->shape_in = (int32_t)cur->arg + 1;
                    p->mode = 7;
                }
                p->target = *cell;
                p->len = self->s3_1e48;
                p->shape_out = (self->s3_1e48 < self->s3_1fe8)
                               ? self->s3_1e48 : self->s3_1fe8;
                Stage3_Write(self, k);
                sv_start = p->start;
                p->len = sv_len - self->s3_1e48;
                self->trk_wr[k] += self->s3_1e48;
            } else {
                *cell = tgt;
            }

            self->trk_rd[k] = sv_wr;
            p->target = *alt;
            p->lead = *cell;
            p->start = *cell;
            v = (self->s3_1fe8 < self->s3_1e44) ? self->s3_1fe8
                                                : self->s3_1e44;
            p->shape_out = v;
            p->shape_in = v;
            if (v > self->s3_1fe8)
                p->shape_in = self->s3_1fe8;
            Stage3_Write(self, k);
            p->target = tgt;
            p->len = sv_len;
            self->trk_wr[k] = sv_wr;

            if (Phone_IsVowel((uint8_t)c_cur)) {
                p->start = self->s3_param_def[k];
                p->mode = 0;
            } else if (c_cur == 'N') {
                p->start = (k == 9) ? sv_start : pole[k - 9];
                p->mode = (k >= 10 && k <= 12) ? 0 : 1;
            } else if (!(c_cur == 'M' && k == 10)) {
                p->mode = 3;
            }

            self->trk_rd[k] = sv_rd;
            p->shape_out = sv_out;
            p->shape_in = sv_in;
            p->lead = sv_lead;
        }

        if (cur->value == 'K' || cur->value == 'G') {
            self->s3_param[12].mode = 6;
            self->s3_param_rate[12] = 0;
        }
        if (!(ctl->flags & 0x20u))
            self->s3_param[0].target -= 3;
        v = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]];
        if ((v == 3 || v == 2) && c_cur == 'G')
            self->s3_param[9].mode = 6;
        return;
    }

    for (i = 0; i < 4; i++)
        self->s3_param[9 + i].start = self->s3_param_def[9 + i];

    k = c_cur - 0x47;
    switch (((uint32_t)k <= 0x23) ? g_op4_sel[k] : 5) {
    case 0:
        self->s3_param[9].mode = 6;
        /* fall through */
    case 1:
        self->s3_param[12].mode = 6;
        self->s3_param_rate[12] = 0;
        break;
    case 2:
        if (Phone_IsVowel(cur->prev->value) && !(ctl->flags & 0x20u)) {
            self->s3_param[9].mode |= 1;
            self->s3_param[9].shape_in = (int32_t)cur->arg + 1;
        }
        break;
    case 3:
        if (c_ctl == 'o') {
            self->s3_param[4].mode = 5;
            self->s3_param[3].mode = 5;
        }
        break;
    case 4:
        break;
    default:
        if (Phone_IsVowel((uint8_t)c_cur)) {
            for (i = 0; i < 4; i++) {
                self->s3_param[9 + i].start = self->s3_param_def[9 + i];
                self->s3_param[9 + i].mode = 6;
                self->s3_param[9 + i].shape_out = (int32_t)(ctl->arg >> 1);
            }
        }
        break;
    }

    if (!(ctl->flags & 0x20u))
        self->s3_param[0].target -= 3;
}


/*
 * The fricatives and affricates.
 *
 * These are the amplitudes rather than the formants: how loud the friction
 * is in each band, and how fast it comes and goes.  The bulk of it is a long
 * list of named clusters -- "ZB", "sC", "XS" and the rest -- each of which
 * the tables would get wrong, so each is given its levels outright.
 */
/* @0x10049570 */
void TV_THISCALL Stage3_Op7(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *scan = st->scan;
    uint8_t c_ctl = ctl->value;
    uint8_t c_cur = cur->value;
    uint8_t c_scan = scan->value;
    uint8_t a, d;
    int32_t i, v;

    if (c_cur == ' ') {
        for (i = 3; i < 8; i++)
            self->s3_param[i].mode = 4;
    }

    if (!(Phone_Attr(px3(c_ctl)) & 4)) {
        self->s3_param[0].target = 0;
        self->s3_param[0].shape_out = 3;
        if (!Phone_IsVowel(c_cur)) {
            self->s3_param[0].mode = 6;
            self->s3_param[0].lead = self->s3_param_def[0];
        } else if (ctl->value == 'S') {
            self->s3_param[0].mode = 4;
        } else {
            self->s3_param[0].lead = 0x28;
            self->s3_param[0].mode = 7;
        }
    }

    if (!(st->p_38 & 1) && !(Phone_Attr(px3(c_ctl) | 0x100) & 0x10) &&
        c_cur != 'J') {
        self->s3_param[1].target -= 0xc;
        if (c_ctl == 'S')
            self->s3_param[1].target -= 3;
        if (self->s3_param[1].target < 0)
            self->s3_param[1].target = 0;
    }

    if (!(Phone_Attr(px3(c_ctl)) & 4)) {
        int32_t soften;

        d = Phone_Attr(px3(c_scan) | 0x100);
        if (d & 2) {
            soften = !(c_ctl == 'S' || !(d & 0x20) || c_ctl == 'X');
        } else if (c_ctl == 'X' && c_scan == 'R') {
            soften = 0;
        } else if (c_cur == 'j' && c_ctl == 'F' && c_scan == 'S') {
            soften = 0;
        } else if (c_ctl == 'S') {
            soften = !(c_scan == ' ' || (Phone_Attr(px3(c_scan)) & 0x40));
        } else {
            soften = 1;
        }
        if (soften)
            self->s3_param[2].target = self->s3_1e54 - 0x14;
    }

    if (c_ctl == 'Z') {
        a = Phone_Attr(px3(c_scan));
        if (a & 2) {
            if (c_scan != 'd') {
                v = self->s3_param_def[0] - 2;
                self->s3_param[0].target = v;
                if (v < 0x32)
                    self->s3_param[0].target = 0x32;
            }
            if (c_cur != ' ') {
                self->s3_param[18].target = 0xa;
                self->s3_param[18].shape_in = 5;
                if (cur->arg < 5u)
                    self->s3_param[18].shape_in = (int32_t)cur->arg;
            }
            if (Phone_Attr(px3(c_cur) | 0x100) & 2) {
                self->s3_param[1].target = 0x40;
                self->s3_param[1].shape_out = 2;
                self->s3_param[7].target = 0x46;
                self->s3_param[4].target = 0x2d;
                self->s3_param[3].target = 0x2d;
                self->s3_param[6].target = 0x48;
                self->s3_param[5].target = 0x48;
            } else if (c_cur == ' ') {
                self->s3_param[7].target = 0x48;
                self->s3_param[5].target = 0x48;
            } else if (c_scan == 'd' && (Phone_Attr(px3(c_cur)) & 0x10)) {
                self->s3_param[1].target = 0x45;
                self->s3_param[5].target = 0x4a;
                self->s3_param[6].target = 0x4a;
                self->s3_param[7].target = 0x4a;
            } else {
                for (i = 5; i < 8; i++)
                    self->s3_param[i].target = 0x44;
            }
        } else if (a & 0x20) {
            for (i = 5; i < 8; i++)
                self->s3_param[i].target = 0x4a;
            if ((c_cur == 'B' || c_cur == 'D' || c_cur == 'G') &&
                (c_scan == 'B' || c_scan == 'D' || c_scan == 'G')) {
                self->s3_param[0].shape_out = (int32_t)(ctl->arg >> 1);
            } else if (Phone_Attr(px3(c_scan)) & 4) {
                v = self->s3_param_def[0] - 2;
                self->s3_param[0].target = v;
                if (v < 0x32)
                    self->s3_param[0].target = 0x32;
            }
            if (Phone_Attr(px3(cur->value)) & 0x10) {
                self->s3_param[1].target = 0x45;
                self->s3_param[1].mode = 4;
            }
        }
    }

    if (c_ctl == 'V' && (Phone_Attr(px3(c_cur) | 0x100) & 2) &&
        (c_scan == 'M' || c_scan == 'N'))
        self->s3_param[1].target = 0x3e;

    if (c_ctl == 'Z') {
        if (c_scan == 'X')
            self->s3_param[2].target = 0x32;
        if ((Phone_Attr(px3(c_cur)) & 0x10) &&
            (c_scan == 'F' || c_scan == 'X')) {
            self->s3_param[1].target = 0x45;
            self->s3_param[1].mode = 4;
        }
        if ((Phone_Attr(px3(c_cur) | 0x100) & 2) && c_scan == 'F')
            self->s3_param[18].target = 0x11;
        if (ctl->prev->value == 'j' && c_scan == 'o') {
            self->s3_param[1].target = 0x3f;
            self->s3_param[19].target = 0xe;
        }
    }

    if (c_ctl == 'X') {
        if (c_cur == 'Z' || c_cur == 'V')
            self->s3_param[1].mode = 6;
        self->s3_param[8].target = 0x37;
        if (ctl->prev->value == 'S' && c_scan == 'R')
            self->s3_param[2].target = 0x49;
        if (ctl->prev->value == 'S' && (c_scan == 'W' || c_scan == 'L')) {
            self->s3_param[2].target = 0x32;
            self->s3_param[6].target = 0x4b;
            self->s3_param[7].target = 0x50;
        }
        if (c_scan == 'E')
            self->s3_param[6].target = 0x46;
        if (c_scan == 'w') {
            self->s3_param[2].target = 0x41;
            self->s3_param[7].target = 0x46;
        }
    }

    if (c_ctl == 'S' && c_scan == 'E' && c_cur == 'w')
        self->s3_param[2].target = 0x37;

    if (c_ctl == 's') {
        if (ctl->prev->value != 'C' &&
            (Phone_Attr(px3(c_scan) | 0x100) & 2)) {
            self->s3_param[1].target = 0x44;
            self->s3_param[2].target = 0x30;
            self->s3_param[4].target = 0x46;
            self->s3_param[5].target = 0x46;
        }
        if (ctl->prev->value != 'C') {
            self->s3_param[6].target = 0x3c;
            self->s3_param[1].target = 0x42;
            self->s3_param[4].target = 0x41;
            self->s3_param[5].target = 0x41;
        }
        if (c_cur == 'C') {
            switch (c_scan) {
            case 'w': case 'c': case 'u':
                self->s3_param[4].target = 0x44;
                self->s3_param[5].target = 0x41;
                break;
            case 'A': case 'o':
                self->s3_param[4].target = 0x46;
                break;
            case 'a': case 'O':
                self->s3_param[4].target = 0x46;
                self->s3_param[5].target = 0x41;
                break;
            case 'v':
                self->s3_param[4].target = 0x3c;
                break;
            case 'R':
                self->s3_param[1].target = 0x44;
                self->s3_param[7].target = 0x37;
                break;
            case 'E':
                if (Phone_Attr(px3(cur->prev->value) | 0x200) & 2) {
                    self->s3_param[1].target = 0x48;
                    self->s3_param[2].target = 0x32;
                }
                break;
            default:
                break;
            }
        }
    }

    if (c_ctl == 'z') {
        if ((Phone_Attr(px3(c_cur) | 0x100) & 2) && c_scan == 'E')
            self->s3_param[1].target = 0x48;
        if (ctl->prev->value == 'J' && c_scan == '|')
            self->s3_param[1].target = 0x44;
        if (ctl->prev->value == 'J' && c_scan == 'r') {
            self->s3_param[1].target = 0x41;
            self->s3_param[4].target = 0x41;
        }
        if (ctl->prev->value == 'J' && c_scan == 'g') {
            self->s3_param[1].target = 0x48;
            self->s3_param[5].target = 0x3e;
            self->s3_param[6].target = 0x46;
        }
    }

    if (c_ctl == 'Z' && c_cur == ' ' &&
        (Phone_Attr(px3(c_scan) | 0x100) & 2))
        self->s3_param[18].target = 0xc;

    if (c_ctl == 'V' && c_cur == ' ' &&
        (Phone_Attr(px3(c_scan) | 0x100) & 2))
        self->s3_param[19].target = 0xa;

    if (c_ctl == 'Z' && c_scan == ' ') {
        d = Phone_Attr(px3(c_cur));
        if ((d & 4) &&
            ((Phone_Attr(px3(c_cur) | 0x100) & 1) || (d & 0x40))) {
            self->s3_param[18].target = 8;
            self->s3_param[19].target = 0xe;
        }
    }

    if (c_ctl == 'z' &&
        (ctl->prev->value == 'J' ||
         (Phone_Attr(px3(ctl->prev->value) | 0x100) & 2)) &&
        c_scan == 'S')
        self->s3_param[4].target = 0x41;

    if (c_ctl == 'Z') {
        if (Phone_Attr(px3(ctl->prev->value)) & 0x10) {
            d = ctl->next->value;
            if ((Phone_Attr(px3(d)) & 8) ||
                (Phone_Attr(px3(d) | 0x80) & 0x40) || d == ' ')
                self->s3_param[4].target = 0x42;
        }
        if ((c_cur == 'B' || c_cur == 'D' || c_cur == 'G') &&
            (Phone_Attr(px3(c_scan) | 0x100) & 2)) {
            self->s3_param[13].target = 0x46;
            self->s3_param[19].target = 0xe;
        }
        if (c_cur == 'E' && c_scan == 'F' && scan->next->value == 'k')
            self->s3_param[1].target = 0x37;
    }

    if (c_ctl == 'F' && ctl->b18 == 1) {
        for (i = 3; i < 9; i++)
            if (self->s3_param[i].target >= 4)
                self->s3_param[i].target -= 4;
    }

    if (c_ctl == 's') {
        if (c_cur == 'N' && (Phone_Attr(px3(c_scan) | 0x100) & 2)) {
            self->s3_param[1].target = 0x44;
            self->s3_param[4].target = 0x44;
            self->s3_param[5].target = 0x44;
        }
        if (((Phone_Attr(px3(c_cur) | 0x200) & 2) ||
             (Phone_Attr(px3(c_cur) | 0x100) & 2) ||
             c_cur == 'L' || c_cur == 'j' || c_cur == 'l') &&
            c_scan == 'E') {
            self->s3_param[1].target = 0x48;
            self->s3_param[2].target = 0x32;
        }
        d = ctl->next->value;
        if ((Phone_Attr(px3(d) | 0x200) & 2) && d != 'r' && d != '4') {
            self->s3_param[1].target = 0x48;
            self->s3_param[2].target = 0x32;
        }
        if (c_cur == 'K' &&
            (c_scan == 'g' || c_scan == '4' || c_scan == 'k')) {
            self->s3_param[1].target = 0x4b;
            self->s3_param[2].target = 0x32;
        }
    }

    if (c_ctl == 'V') {
        if (c_cur == ' ' && (Phone_Attr(px3(c_scan) | 0x100) & 2))
            self->s3_param[19].target = 0xe;
        if ((Phone_Attr(px3(c_scan) | 0x100) & 2) ||
            (ctl->next->value == 'L' &&
             (Phone_Attr(px3(ctl->next->next->value) | 0x100) & 2)))
            self->s3_param[0].target = 0x2d;
    }

    if (c_ctl == 'F' && c_cur == 'f' && c_scan == ' ')
        self->s3_param[2].target = 0x2d;
}
/*
 * The second parameter pass: a handful of cluster rules.
 *
 * Mostly the second and third formants, which move a long way across a
 * cluster and need nudging when the tables would leave them where the
 * previous phoneme put them.
 */
/* @0x1004be40 */
void TV_THISCALL Stage3_Op12(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    int32_t c_cur = px3(cur->value);
    int32_t c_ctl = px3(st->ctl->value);
    uint8_t a;

    if ((Phone_Attr(c_cur | 0x100) & 8) || (Phone_Attr(c_cur | 0x200) & 2)) {
        int32_t set = 0;

        if (c_ctl == 'K') {
            a = st->scan->value;
            if (!(Phone_Attr(px3(a) | 0x100) & 2) && a != 'p')
                set = 1;
        } else if (c_ctl == '~') {
            set = 1;
        }
        if (set) {
            self->s3_param[10].target = 0x6a4;
            self->s3_param[11].target = 0x76c;
        }
        if ((Phone_Attr(c_ctl | 0x100) & 2) &&
            !(Phone_Attr(c_cur | 0x100) & 2)) {
            self->s3_param[11].shape_out = 5;
            if (c_ctl == 'E' && c_cur == 'R')
                self->s3_param[0].shape_out = 6;
        }
    }

    if (c_ctl == 'R' && (Phone_Attr(c_cur | 0x100) & 0x10)) {
        self->s3_param_rate[10] = 0x19a0;
    } else if ((Phone_Attr(c_cur | 0x180) & 0x40) &&
               (c_ctl == 'Z' || c_ctl == 'n' || c_ctl == 'q' ||
                c_ctl == 't')) {
        self->s3_param[10].target = 0x76c;
    }

    if (c_ctl == 'd' && st->scan->value == 'b')
        self->s3_param[9].target = 0xb6;

    if (st->ctl->value == 'B' &&
        (Phone_Attr(px3(cur->value) | 0x100) & 2) &&
        st->ctl->next->value == 'p')
        self->s3_param[0].mode = 4;

    if (c_ctl == 'y' && cur->value == 'L')
        self->s3_param[9].shape_in = 5;

    if (c_ctl == 'J')
        self->s3_param[1].target = 0x40;
}

/*
 * Bring the aspiration forward over the frames already written.
 *
 * A voiced phoneme after an aspirated stop starts while the aspiration is
 * still dying away, so the stop's own run is shortened by s3_1e34 frames and
 * the aspiration amplitude is faded back into them.
 */
/* @0x100055b0 */
void TV_THISCALL Stage3_Op15(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t back = self->s3_1e34;
    int32_t pos = self->trk_wr[2];
    uint8_t *buf;

    if (back > pos)
        return;
    if (st->voice == 8 && self->sample_rate == 0x1f40)
        return;

    pos -= back;
    buf = self->trk_buf[2];
    self->trk_wr[2] = pos;
    Track_Nudge(self, buf, 2, pos, 3, pos - self->trk_rd[2],
                self->s3_1e58 - (int32_t)buf[pos & 0xff]);
    self->s3_param[2].len += self->s3_1e34;
    self->s3_param[2].shape_out = 3;
    self->s3_param_def[2] = self->s3_1e58;

    if (st->ctl->value == ' ') {
        pos = self->trk_wr[0];
        Track_Nudge(self, self->trk_buf[0], 0, pos, 0xa - self->s3_1e34,
                    pos - self->trk_rd[0], -4);
    }
    if (Phone_Attr(px3(st->ctl->value) | 0x100) & 1)
        self->s3_param[2].mode = 5;
}

/* A voiced phoneme after an aspirated stop: start it early. */
/* @0x10048100 */
void TV_THISCALL Stage3_Op1(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t c_ctl = px3(st->ctl->value);
    int32_t c_cur = px3(st->cur->value);
    int32_t v;

    if ((Phone_Attr(c_cur) & 2) && !(Phone_Attr(c_ctl | 0x100) & 1)) {
        self->s3_1e34 = 1;
        if (Phone_Attr(c_ctl) & 0x40) {
            v = self->trk_wr[1];
            if (v >= 2) {
                self->s3_param[1].len += 2;
                self->trk_wr[1] = v - 2;
            }
        }
        Stage3_Op15(self);
    }

    if (st->ctl->value != 'S' || st->scan->value != ' ' ||
        !(Phone_Attr(px3(st->cur->value) | 0x100) & 2))
        self->s3_param[17].mode = 4;
    if (st->ctl->value != 'S')
        self->s3_param[0].mode = 5;
}

/* A second phoneme grouping, used to pick how fast the formants move. */
/* @0x100ef6d8 */ extern const tv_ref g_phone_group;
/* @0x100ef278 */ extern const uint8_t g_group_shape[];

/*
 * How long the formants have to travel into this phoneme.
 *
 * The curve every formant follows comes from the group the phoneme before it
 * belongs to; the first formant gets half of it, since it moves least.
 */
/* @0x1004a0e0 */
void TV_THISCALL Stage3_Op8(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t c_cur = px3(st->cur->value);
    int32_t c_ctl = px3(st->ctl->value);
    int32_t i, v;
    uint8_t a;

    if (!(Phone_Attr(c_cur | 0x100) & 1)) {
        for (i = 0; i < 10; i++)
            self->s3_param[i].mode = 5;
        if (st->ctl->value == 'T')
            self->s3_param[9].mode = 7;
        if (st->ctl->value == 'M' && st->cur->value == 'z')
            self->s3_param[9].mode = 7;
    }

    a = Phone_Attr(c_ctl);
    if (a & 4) {
        if (a & 0x20)
            self->s3_param[0].mode = 6;
        else if (a & 0x10)
            self->s3_param[0].mode = 4;
    }

    if (Phone_Attr(c_cur | 0x100) & 0x40) {
        self->s3_param[11].mode = 5;
        self->s3_param[10].mode = 5;
    }

    if (Phone_Attr(c_cur | 0x200) & 1)
        v = 5;
    else
        v = (int32_t)g_group_shape[TV_REF(uint8_t, g_phone_group)[c_ctl]];
    for (i = 9; i < 17; i++)
        self->s3_param[i].shape_out = v;
    self->s3_param[9].shape_out = self->s3_param[9].shape_out / 2 + 1;
}

/*
 * The same, but taking the curve from the phoneme just finished and choosing
 * how each parameter travels from what kind of sound it was.
 */
/* @0x100481b0 */
void TV_THISCALL Stage3_Op2(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t c_cur = px3(st->cur->value);
    int32_t i;

    for (i = 9; i < 17; i++)
        self->s3_param[i].shape_out =
            (int32_t)g_group_shape[TV_REF(uint8_t, g_phone_group)[px3(st->cur->value)]];
    self->s3_param[9].shape_out = self->s3_param[9].shape_out / 2 + 1;

    if (Phone_Attr(c_cur | 0x80) & 8) {
        self->s3_param[2].mode = 6;
        self->s3_param[1].mode = 6;
        self->s3_param[0].mode = 6;
        for (i = 3; i < 9; i++)
            self->s3_param[i].mode = 4;
    } else if (Phone_Attr(c_cur) & 0x20) {
        for (i = 1; i < 9; i++)
            self->s3_param[i].mode = 4;
        if (c_cur == 'T' && st->ctl->value == 'p') {
            self->s3_param[1].mode = 6;
            self->s3_param_rate[2] = 0;
            self->s3_param_rate[1] = 0;
            self->s3_param[2].mode = 6;
        }
    } else {
        for (i = 0; i < 9; i++)
            self->s3_param[i].mode = 6;
    }

    if ((Phone_Attr(c_cur) & 0x10) &&
        (Phone_Attr(px3(st->ctl->value)) & 4))
        self->s3_param[0].mode = 4;

    if (c_cur == 'J' || c_cur == 'B' || c_cur == 'D' || c_cur == 'G') {
        if (Phone_Attr(px3(st->cur->prev->value)) & 4) {
            self->s3_param[0].start = 0x34;
            self->s3_param[0].lead = 0x34;
        }
    }
}

/*
 * How far the formants have to travel out of this phoneme.
 *
 * A sound the tongue has to leave slowly -- a liquid, a nasal -- makes the
 * formants take longer, and the ones that hardly move at all get a limit on
 * how fast they may.
 */
/* @0x10049100 */
void TV_THISCALL Stage3_Op5(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t c_ctl = px3(st->ctl->value);
    int32_t c_cur = px3(st->cur->value);
    uint8_t a = Phone_Attr(c_ctl | 0x100);
    int32_t i, v;

    if ((a & 0x40) && (Phone_Attr(c_cur) & 2) &&
        !(Phone_Attr(c_cur) & 0x10)) {
        self->s3_param_rate[11] = 0x7350;
        self->s3_param_rate[10] = 0x7350;
        self->s3_param_rate[9] = 0x7350;
    }

    if (c_ctl != 'l' && st->scan->value != ' ') {
        v = 9;
        if (Phone_Attr(c_ctl | 0x180) & 1) {
            v = 7;
            if (Phone_Attr(c_cur | 0x180) & 1)
                v = 5;
        }
        for (i = 9; i < 17; i++)
            self->s3_param[i].shape_out = v;
    }

    if (a & 8) {
        for (i = 10; i < 13; i++)
            self->s3_param[i].shape_out = 6;
    }

    if (c_ctl == 'n') {
        if (st->scan->value == ' ' && st->scan->next->value == ' ') {
            for (i = 10; i < 13; i++)
                self->s3_param[i].shape_out = 4;
        }
        if (st->cur->value == 'D')
            self->s3_param[10].mode = 6;
    }

    if (c_ctl == 'H') {
        if (Phone_Attr(px3(st->scan->value) | 0x200) & 2)
            self->s3_param[2].target = 0x37;
        if (st->scan->value == 'g' && st->cur->value == 's' &&
            st->cur->prev->value == 'C')
            self->s3_param[2].target = 0x43;
    }
}

/*
 * The per-phoneme parameter tables, all indexed by the phoneme's class.
 * Eight of them give the formants and their bandwidths; the amplitudes come
 * through a second level of indirection because only the classes above 0x1e
 * -- the ones that make a noise of their own -- have them.
 */
/* @0x100eece8 */ extern const uint8_t g_pt_f1[64];
/* @0x100eed28 */ extern const uint8_t g_pt_f2[64];
/* @0x100eed68 */ extern const uint8_t g_pt_f3[64];
/* @0x100eeda8 */ extern const uint8_t g_pt_f4[64];
/* @0x100eede8 */ extern const uint8_t g_pt_b1[64];
/* @0x100eee28 */ extern const uint8_t g_pt_b2[64];
/* @0x100eee68 */ extern const uint8_t g_pt_b3[64];
/* @0x100eeea8 */ extern const uint8_t g_pt_av[64];
/* @0x100ef158 */ extern const tv_ref g_pt_a1;
/* @0x100ef180 */ extern const tv_ref g_pt_a2;
/* @0x100ef1a8 */ extern const tv_ref g_pt_a3;
/* @0x100ef1d0 */ extern const tv_ref g_pt_a4;
/* @0x100ef1f8 */ extern const tv_ref g_pt_a5;
/* @0x100ef220 */ extern const tv_ref g_pt_a6;
/* @0x100ef248 */ extern const tv_ref g_pt_a7;
/* @0x100ef270 */ extern const tv_ref g_pt_a8;
/* @0x100ef330 */ extern const tv_ref g_pt_a16;
/* The second set, for the classes below 0x13. */
/* @0x100eefd8 */ extern const uint8_t g_at_f1[];
/* @0x100eeff0 */ extern const uint8_t g_at_f2[];
/* @0x100ef008 */ extern const uint8_t g_at_f3[];
/* @0x100ef020 */ extern const uint8_t g_at_f4[];
/* @0x100ef038 */ extern const uint8_t g_at_b1[];
/* @0x100ef050 */ extern const uint8_t g_at_b2[];
/* @0x100ef068 */ extern const uint8_t g_at_b3[];
/* @0x100ef120 */ extern const uint8_t g_pt_open[];
/* The transition context each class leaves behind and expects. */
/* @0x100eeee8 */ extern const int32_t g_ctx_next[];
/* @0x100ef080 */ extern const int32_t g_ctx_prev[];
/* How fast a parameter may move, by the pair of contexts. */
/* @0x100ef290 */ extern const int32_t g_rate_by_ctx[];
/* The curve each parameter starts out travelling along. */
/* @0x100ef310 */ extern const uint8_t g_shape_init[22];
/* The burst a stop makes, by its place and whether a vowel follows. */
/* @0x100ef377 */ extern const uint8_t g_stop_burst[];
/* @0x1002d9cc */ extern const uint8_t g_stop_rel_kind[17];
/* Per-voice offsets. */
/* @0x100b53c8 */ extern const int32_t g_voice_f4[];
/* @0x100b5310 */ extern const uint8_t g_voice_p18[];
/* @0x100b5320 */ extern const uint8_t g_voice_p19[];
/* @0x100b5330 */ extern const uint8_t g_voice_p20[];
/* @0x100b5340 */ extern const uint8_t g_voice_p21[];

/*
 * Load this phoneme's parameter targets.
 *
 * Everything the synthesizer needs for one phoneme comes out of the tables
 * above, read with the phoneme's class: four formants and their bandwidths,
 * the amplitudes of the cascade and the parallel branch, the pitch.  What
 * follows is the part the tables cannot hold -- how a stop's burst and
 * release depend on what it sits between -- and then the defaults every
 * parameter starts from before the rules run.
 */
/* @0x1002d0d0 */
void TV_THISCALL Stage3_Targets(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *scan;
    int32_t c_ctl = px3(ctl->value);
    uint8_t c_cur = cur->value;
    int32_t cls_ctl = (int32_t)(int8_t)TV_REF(uint8_t, g_phone_class)[c_ctl];
    int32_t cls_cur = (int32_t)TV_REF(uint8_t, g_phone_class)[px3(c_cur)];
    int32_t voice = st->voice;
    int32_t i, v, kind, cls_scan, idx;
    uint8_t a, a_scan, c, d;

    if ((uint32_t)cls_cur < 0x13 && c_cur != 'u' && ctl->value != 'j')
        v = g_ctx_prev[cls_cur];
    else
        v = self->s3_1e30;
    self->s3_1e2c = v;
    self->s3_1e24 = self->s3_1e28;
    self->s3_1e30 = g_ctx_next[cls_ctl];
    self->s3_1e54 = 0x33;
    self->s3_1e5c[2] = 0;
    self->s3_1e5c[1] = 0;
    self->s3_1e58 = 0x33;
    self->s3_1e5c[0] = 0;

    a = Phone_Attr(c_ctl | 0x100);
    if (a & 2)
        self->s3_1e28 = 0;
    else if (a & 1)
        self->s3_1e28 = 3;
    else
        self->s3_1e28 = (Phone_Attr(c_ctl) & 2) ? 1 : 2;

    self->s3_param[9].target  = (int32_t)g_pt_f1[cls_ctl] << 2;
    self->s3_param[10].target = ((int32_t)g_pt_f2[cls_ctl] << 3) + 0x1f4;
    self->s3_param[11].target = (int32_t)g_pt_f3[cls_ctl] << 4;
    self->s3_param[12].target = ((int32_t)g_pt_f4[cls_ctl] << 4) +
                                g_voice_f4[voice];
    self->s3_param[13].target = (int32_t)g_pt_b1[cls_ctl] * 2;
    self->s3_param[14].target = (int32_t)g_pt_b2[cls_ctl] * 2;
    self->s3_param[15].target = (int32_t)g_pt_b3[cls_ctl] * 2;
    self->s3_param[0].target  = (int32_t)g_pt_av[cls_ctl];
    self->s3_param[17].target = (int32_t)ctl->b15 * 2;
    self->s3_param[18].target = (int32_t)g_voice_p18[voice];
    self->s3_param[19].target = (int32_t)g_voice_p19[voice];
    self->s3_param[20].target = (int32_t)g_voice_p20[voice];
    self->s3_param[21].target = (int32_t)g_voice_p21[voice];

    if (cls_ctl < 0x1e) {
        /* a class with no noise of its own */
        self->s3_param[1].target = 0;
        self->s3_param[2].target = 0;
        self->s3_param[3].target = 0x3c;
        self->s3_param[4].target = 0x3c;
        self->s3_param[5].target = 0x3c;
        self->s3_param[6].target = 0x3c;
        self->s3_param[7].target = 0x3c;
        self->s3_param[8].target = 0;
        if (cls_ctl < 0x13) {
            self->s3_1e44 = (int32_t)g_pt_open[cls_ctl];
            self->s3_alt[0] = (int32_t)g_at_f1[cls_ctl] << 2;
            self->s3_alt[1] = ((int32_t)g_at_f2[cls_ctl] << 3) + 0x1f4;
            self->s3_alt[2] = (int32_t)g_at_f3[cls_ctl] << 4;
            self->s3_alt[3] = (int32_t)g_at_f4[cls_ctl] << 4;
            self->s3_alt[4] = (int32_t)g_at_b1[cls_ctl] * 2;
            self->s3_alt[5] = (int32_t)g_at_b2[cls_ctl] * 2;
            self->s3_alt[6] = (int32_t)g_at_b3[cls_ctl] * 2;
        }
        if (c_ctl == 'u' && st->scan->value == 'j')
            self->s3_1e30 = 0;
        goto defaults;
    }

    self->s3_param[1].target = (int32_t)TV_REF(uint8_t, g_pt_a1)[cls_ctl];
    self->s3_param[2].target = (int32_t)TV_REF(uint8_t, g_pt_a2)[cls_ctl];
    self->s3_param[3].target = (int32_t)TV_REF(uint8_t, g_pt_a3)[cls_ctl];
    self->s3_param[4].target = (int32_t)TV_REF(uint8_t, g_pt_a4)[cls_ctl];
    self->s3_param[5].target = (int32_t)TV_REF(uint8_t, g_pt_a5)[cls_ctl];
    self->s3_param[6].target = (int32_t)TV_REF(uint8_t, g_pt_a6)[cls_ctl];
    self->s3_param[7].target = (int32_t)TV_REF(uint8_t, g_pt_a7)[cls_ctl];
    self->s3_param[8].target = (int32_t)TV_REF(uint8_t, g_pt_a8)[cls_ctl];

    if (cls_ctl < 0x22 || cls_ctl >= 0x2b)
        goto defaults;

    scan = st->scan;
    if (cls_ctl >= 0x26 && cls_ctl < 0x29) {
        /* an affricate: the burst depends on what follows */
        kind = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(scan->value)]];
        if (kind == 8 || kind == 9)
            goto defaults;
        self->s3_1e5c[1] = 0x7f;
        if (kind == 6 && ctl->value != 't')
            self->s3_1e5c[1] = (ctl->value == 'J') ? 0x36 : 0x3c;
        self->s3_1e5c[2] = (ctl->value == 'J') ? 0 : 0x33;
        if (kind == 6 && ctl->value == 'C')
            self->s3_1e5c[2] = 0;
        self->s3_1e5c[1] = 0x7f;
        if (kind == 6 && ctl->value != 't')
            self->s3_1e5c[1] = (ctl->value == 'J') ? 0 : 0x46;
        self->s3_1e5c[0] = 0x7f;
        if (kind == 6 && ctl->value == 'J')
            self->s3_1e5c[0] = 0;
        self->s3_1e38 = 0x7f;
        self->s3_1e3c = (cls_ctl == 0x26) ? 0x7f : 1;
        goto defaults;
    }

    /* a stop: its burst comes from a small table of its own */
    idx = cls_ctl - 0x22;
    if (cls_ctl >= 0x29)
        idx -= 3;
    idx <<= 3;
    if (!(Phone_Attr(px3(scan->value) | 0x100) & 2))
        idx += 4;
    for (i = 1; i <= 3; i++)
        self->s3_1e5c[i - 1] = (int32_t)g_stop_burst[idx + i];
    self->s3_param[8].target = (int32_t)g_stop_burst[idx + 4];

    d = scan->value;
    kind = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(d)]];
    if (kind == 8 || kind == 9) {
        c = ctl->value;
        if (c == 'P' || c == 'K' || c == 'B' || c == 'G' || c == 'T' ||
            c == 'D') {
            int32_t mute = 1;

            if (c == 'K' &&
                ((Phone_Attr(px3(d)) & 0x10) || d == 'Q' || d == 't'))
                mute = 0;
            if (mute) {
                self->s3_1e5c[0] = 0x7f;
                self->s3_1e5c[1] = 0x7f;
                self->s3_1e5c[2] = 0x7f;
                self->s3_param[8].target = 0;
            }
        }
    }

    if (Phone_Attr(px3(ctl->value)) & 4)
        self->s3_1e38 = 0x7f;
    else
        self->s3_1e38 = (int32_t)(ctl->d10 & 0xffu);

    self->s3_1e3c = 0;
    a_scan = Phone_Attr(px3(d) | 0x100);
    if ((a_scan & 1) && !(Phone_Attr(px3(d)) & 0x10) &&
        (!(Phone_Attr(px3(cur->value) | 0x100) & 1) ||
         (Phone_Attr(px3(cur->value)) & 0x10))) {
        /* the release of a stop into another stop */
        i = px3(ctl->value) - 0x44;
        if ((uint32_t)i > 0x10) {
            self->s3_1e3c = 0x7f;
            goto defaults;
        }
        switch (g_stop_rel_kind[i]) {
        case 0:
            if (cur->value == 'j' && (d == 'B' || d == 'G'))
                self->s3_1e3c = 1;
            break;
        case 1:
            if (d == 'P') {
                self->s3_1e3c = 1;
                self->s3_1e5c[1] = 0x3c;
                self->s3_1e5c[2] = 0x30;
            } else {
                self->s3_1e3c = 0x7f;
            }
            break;
        case 2:
            if (!(Phone_Attr(px3(d)) & 4)) {
                self->s3_1e3c = 1;
                self->s3_1e5c[1] = 0x3c;
                self->s3_1e5c[2] = 0x30;
            } else {
                self->s3_1e3c = 0x7f;
            }
            break;
        default:
            self->s3_1e3c = 0x7f;
            break;
        }
        goto defaults;
    }

    c = ctl->value;
    if (c == 'K' && (Phone_Attr(px3(d)) & 0x40)) {
        self->s3_1e3c = 1;
    } else if (c == 'G') {
        if (!(scan->flags & 0x20u) && (a_scan & 2))
            self->s3_1e3c = 1;
        else
            self->s3_1e3c = 2;
    } else if (c == 'T' && (Phone_Attr(px3(d)) & 0x40)) {
        self->s3_1e3c = 0x7f;
    } else if (c == 'D') {
        if ((Phone_Attr(px3(d)) & 2) && !(a_scan & 2))
            self->s3_1e3c = 0x7f;
        else if (cur->value == ' ')
            self->s3_1e3c = 2;
        else
            self->s3_1e3c = 1;
    } else if (!(Phone_Attr(px3(c) | 0x100) & 0x10) && d != 'p') {
        self->s3_1e3c = 2;
    } else {
        self->s3_1e3c = 1;
    }

defaults:
    if (cls_ctl >= 0x35)
        self->s3_param[16].target = (int32_t)TV_REF(uint8_t, g_pt_a16)[cls_ctl] * 4 + 0xc0;
    else
        self->s3_param[16].target = 0xf8;

    v = g_rate_by_ctx[self->s3_1e24 * 4 + self->s3_1e28];
    for (i = 0; i < 22; i++) {
        self->s3_param[i].shape_out = (int32_t)g_shape_init[i];
        self->s3_param[i].mode = 7;
        self->s3_param[i].len = (int32_t)st->ctl->arg;
        self->s3_param_rate[i] = v;
    }
    self->s3_param[20].mode = 4;
    self->s3_param[21].mode = 4;

    /* and a look-ahead at where the next phoneme wants the formants */
    cls_scan = (int32_t)(int8_t)TV_REF(uint8_t, g_phone_class)[px3(st->scan->value)];
    self->s3_next[9]  = (int32_t)g_pt_f1[cls_scan] << 2;
    self->s3_next[10] = ((int32_t)g_pt_f2[cls_scan] << 3) + 0x1f4;
    self->s3_next[11] = (int32_t)g_pt_f3[cls_scan] << 4;
    if (cls_scan >= 0x22) {
        self->s3_next[13] = (int32_t)g_pt_b1[cls_scan] * 2;
        self->s3_next[14] = (int32_t)g_pt_b2[cls_scan] * 2;
        self->s3_next[15] = (int32_t)g_pt_b3[cls_scan] * 2;
    }
}

/*
 * The voiced stops and the fricatives before them.
 *
 * A voiced stop keeps a little voicing going through its closure, which
 * these rules size; the "s" of an affricate is shortened before a nasal;
 * and the parallel branch is muted where the closure is complete.
 */
/* @0x10049260 */
void TV_THISCALL Stage3_Op6(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *scan = st->scan;
    Node *cur = st->cur;
    int32_t c_ctl = px3(st->ctl->value);
    int32_t c_scan = px3(scan->value);
    uint8_t a, b;
    Node *n;

    if (Phone_Attr(c_ctl | 0x180) & 4) {
        int32_t shortened = 0;

        if (c_ctl == 's') {
            n = st->ctl->next;
            b = n->value;
            if ((b == '|' || b == '@') && n->next->value == 'N') {
                self->s3_param[1].shape_out -= 2;
                shortened = 1;
            } else if (!(Phone_Attr(c_scan | 0x100) & 0x20) &&
                       !(Phone_Attr(c_scan | 0x180) & 0x20)) {
                self->s3_param[1].shape_out -= 2;
                shortened = 1;
            }
        }
        if (!shortened)
            self->s3_param[1].shape_out = 6;

        if ((Phone_Attr(c_ctl | 0x80) & 8) &&
            !(Phone_Attr(px3(cur->value)) & 4))
            self->s3_param[0].target = 0;
    }

    if (Phone_Attr(c_scan) & 2) {
        self->s3_1e40 = 2;
        if (scan->arg <= 2u)
            self->s3_1e40 = (int32_t)scan->arg - 1;
        if (Phone_Attr(c_ctl) & 0x20)
            self->s3_1e40 = 0;
        if ((c_ctl == 'X' && c_scan == 'R') || c_ctl == 'S')
            self->s3_1e40 = 0;
        if (c_ctl != 's')
            self->s3_param[1].len += self->s3_1e40;
    }

    a = Phone_Attr(c_ctl);
    if (!(a & 4) || !(a & 0x20))
        goto tail;

    /* a voiced stop */
    if (!((c_ctl == 'D' &&
           ((Phone_Attr(c_scan | 0x100) & 2) || c_scan == 'n')) ||
          (c_ctl == 'J' && c_scan == 'z') ||
          (scan->flags & 0x20u))) {
        self->s3_param[5].target = 0;
        self->s3_param[4].target = 0;
    }

    if (!((Phone_Attr(c_scan | 0x100) & 2) ||
          (c_ctl == 'G' && c_scan == 'Y') || c_scan == 'Z' ||
          (c_ctl == 'D' && c_scan == 'n')))
        self->s3_param[0].target -= 0x14;

    b = Phone_Attr(px3(cur->value));
    if (((b & 4) || (c_ctl == 'D' && c_scan == '|')) && c_ctl != 'q') {
        if (!(b & 2))
            self->s3_param[0].target -= 0x1e;
    } else {
        self->s3_param[0].target = 0;
    }

    if (c_ctl != 'q' && (Phone_Attr(px3(cur->value)) & 4)) {
        self->s3_param[0].target = 0x32;
        self->s3_param[0].shape_out = (int32_t)st->ctl->arg;
        goto tail;
    }

    if ((c_ctl == 'B' || c_ctl == 'G') && cur->value == 'S') {
        self->s3_param[0].target = 0;
        goto tail;
    }
    if (!(Phone_Attr(c_scan | 0x100) & 2))
        goto tail;
    if (c_ctl == 'B') {
        self->s3_param[0].target = 0x32;
        goto tail;
    }
    if (c_ctl != 'D')
        goto tail;
    if (c_scan == '|') {
        self->s3_param[0].target = 0;
        goto tail;
    }
    b = cur->value;
    if (b != ' ' && b != 'S') {
        if (scan->flags & 0x20u)
            self->s3_param[0].target = 0x2b;
        goto tail;
    }
    if ((Phone_Attr(px3(b)) & 1) && scan->value != 'p') {
        self->s3_param[5].target = 0;
        self->s3_param[4].target = 0;
    }

tail:
    if (c_scan == 's' && scan->next->value == 'R' &&
        (st->ctl->flags & 0x20u)) {
        self->s3_1e5c[2] = 0;
        self->s3_param[7].target = 0x41;
        self->s3_param[4].target = 0x3c;
        self->s3_1e5c[1] = 0x3c;
    }
    if (c_ctl == 'F' && cur->value == ' ')
        self->s3_param[8].target = 0x3c;
    if (c_ctl == 'V')
        self->s3_param[8].target = 0x38;
}

/* @0x10050740 */
void TV_THISCALL Stage3_Voiced(Engine *self);

/*
 * A liquid or a glide next to a consonant.
 *
 * These are the sounds whose formants move furthest, so most of this decides
 * how long they have to get there, and when the amplitudes should go back to
 * the phoneme's own defaults rather than carrying over.
 */
/* @0x10048310 */
void TV_THISCALL Stage3_Op3(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *n;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    uint8_t a_cur = Phone_Attr(c_cur);
    uint8_t a_ctl = Phone_Attr(c_ctl | 0x100);
    uint8_t a_cur3, b;
    int32_t i, v, kind, restore = 0;

    if (((c_ctl == 'R' || c_ctl == 'L' || c_ctl == 'W') &&
         (a_cur & 0x40) && (a_cur & 4)) ||
        !((a_cur & 4) || c_cur == ' ' || c_cur == 'H' ||
          (cur->flags & 0x40u) ||
          ((ctl->flags & 0x20u) && (a_ctl & 2) && c_cur == 'D')))
        Stage3_Voiced(self);

    if (!(a_ctl & 2))
        self->s3_1e38 = 0x7f;

    if ((a_cur & 0x60) && c_cur != 's')
        self->s3_param[1].len -= self->s3_1e40;

    if (!Phone_IsVowel((uint8_t)c_ctl)) {
        a_cur3 = Phone_Attr(c_cur | 0x180);
        if (a_cur3 & 1) {
            v = 7;
            if ((Phone_Attr(c_ctl | 0x180) & 0x20) && c_cur == 'Y')
                v = 0xb;
            for (i = 9; i < 17; i++)
                self->s3_param[i].shape_out = v;
            if (Phone_Attr(c_cur | 0x100) & 8)
                self->s3_param[11].shape_out = 9;
        }
        if (!(a_ctl & 0x10) &&
            ((Phone_Attr(c_cur | 0x200) & 4) || (a_cur3 & 0x40))) {
            self->s3_param_rate[9] = 0x2cd8;
            self->s3_param_rate[10] = 0x2cd8;
            self->s3_param_rate[11] = 0x2cd8;
        }
        if ((Phone_Attr(c_ctl | 0x180) & 0x20) && (a_cur3 & 4))
            self->s3_param[10].shape_out += 5;
    }

    if (c_cur == 'T' && ((a_ctl & 2) || c_ctl == 'n')) {
        restore = 1;
    } else if (c_cur == 'K') {
        kind = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]];
        if (kind == 3 || kind == 2 || c_ctl == 'a' || c_ctl == 'l' ||
            c_ctl == 'W' || c_ctl == 'L' || c_ctl == 'Y' || c_ctl == 'p')
            restore = 1;
    }
    if (!restore && c_cur == 'P') {
        if (c_ctl == 'i') {
            restore = 1;
        } else if (c_ctl == 'R') {
            n = st->scan;
            if ((n->flags & 0x20u) &&
                (Phone_Attr(px3(n->value) | 0x100) & 2))
                restore = 1;
        }
    }
    if (!restore && c_cur == 'X' && (c_ctl == 'R' || (a_ctl & 2)))
        restore = 1;
    if (restore) {
        for (i = 3; i < 9; i++)
            self->s3_param[i].target = self->s3_param_def[i];
    }

    if ((c_cur == 'P' || c_cur == 'T' || c_cur == 'K') && c_ctl == 'p')
        self->s3_param[0].target = 0;

    if ((a_ctl & 2) && !(ctl->flags & 0x20u)) {
        b = ctl->next->value;
        if (b != '&' && b != '%') {
            n = st->scan;
            if ((Phone_Attr(px3(n->value) | 0x100) & 2) &&
                (n->flags & 0x20u))
                self->s3_param[0].target -= 2;
        }
    }
    if ((a_ctl & 2) && (ctl->flags & 0x20u)) {
        b = ctl->prev->value;
        if (b != '&' && b != '%') {
            n = st->cur;
            if ((Phone_Attr(px3(n->value) | 0x100) & 2) &&
                !(n->flags & 0x20u))
                self->s3_param[0].target += 2;
        }
    }

    if ((c_ctl == 'R' || c_ctl == 'L') && ctl->prev->value == 'P')
        self->s3_param[8].target = 0;
}

/* Packed bit tables, one per kind of table the caller asks for. */
/* @0x10039cec */ extern const uint8_t g_bits_sel[24];
/* @0x100cef70 */ extern const uint8_t g_bits0[];
/* @0x1012c0be */ extern const uint8_t g_bits1[];
/* @0x1010ad82 */ extern const uint8_t g_bits2[];
/* @0x100ae526 */ extern const uint8_t g_bits3[];
/* @0x10101b1b */ extern const uint8_t g_bits4[];
/* @0x100c7d4f */ extern const uint8_t g_bits5[];
/* @0x100f72cb */ extern const uint8_t g_bits6[];

/* Count the bits set across the first n bytes of one of those tables. */
/* @0x10039bb0 */
int32_t TV_STDCALL Bits_Count(int32_t which, int32_t n)
{
    const uint8_t *tab;
    int32_t total = 0, i, v, b;
    /* The original leaves this from the previous row when "which" is out of
     * range, which the tables never are. */
    int32_t byte = 0;

    if (n > 0x91)
        n = 0x91;
    for (i = 0; i < n; i++) {
        v = 0x100;
        if ((uint32_t)which <= 0x17) {
            switch (g_bits_sel[which]) {
            case 0: tab = g_bits0; break;
            case 1: tab = g_bits1; break;
            case 2: tab = g_bits2; break;
            case 3: tab = g_bits3; break;
            case 4: tab = g_bits4; break;
            case 5: tab = g_bits5; break;
            default: tab = g_bits6; break;
            }
            byte = (int32_t)tab[which * 145 + i];
        }
        b = byte;
        while (b != 0) {
            v /= 2;
            if ((uint32_t)b >= (uint32_t)v) {
                b -= v;
                total++;
            }
        }
    }
    return total;
}

/*
 * Find which entry of each of four tables allows this pair of phonemes.
 *
 * Each table is a list of entries, and each entry names a phoneme and the
 * set of phonemes it may be followed by.  What comes back is the index of
 * the first entry that allows the pair, or the table's length when none do.
 */
/* @0x10050670 */
void TV_STDCALL Stage3_FindPair(int32_t *out, uint8_t c1, uint8_t c2,
                                const int32_t *counts,
                                const tv_ref *tab)
{
    const S3Pair *p;
    const uint8_t *s;
    int32_t slot, base = 0, i;
    uint8_t found;

    for (slot = 0; slot < 4; slot++) {
        found = 0;
        if (slot != 0)
            base += counts[slot - 1];
        for (i = 0; i < counts[slot] && !found; i++) {
            p = TV_REF(S3Pair, tab[base + i]);
            for (; p->ch != 0 && !found; p++) {
                if (p->ch != c1)
                    continue;
                for (s = TV_REF(uint8_t, p->set); *s != 0 && !found; s++) {
                    if (*s == c2) {
                        found = 1;
                        out[slot] = i;
                    }
                }
            }
        }
        if (!found)
            out[slot] = counts[slot];
    }
}

/* A bit per position within a byte of the packed tables. */
/* @0x100bf7f0 */ extern const uint8_t g_bit_mask[8];

/*
 * Where in the packed table this bit sits.
 *
 * The tables store one bit per slot; this says how many set bits come before
 * the one asked for, which is its index among the entries that exist, or -1
 * when the slot is empty.
 */
/* @0x10038700 */
int32_t TV_STDCALL Bits_Rank(int32_t bit, int32_t row, int32_t which)
{
    const uint8_t *tab;
    int32_t idx = bit + 34 * row;
    int32_t byte_i = idx / 8;
    int32_t bit_i = idx - byte_i * 8;
    int32_t v = 0, n = 0, i;

    if ((uint32_t)which <= 0x17) {
        switch (g_bits_sel[which]) {
        case 0: tab = g_bits0; break;
        case 1: tab = g_bits1; break;
        case 2: tab = g_bits2; break;
        case 3: tab = g_bits3; break;
        case 4: tab = g_bits4; break;
        case 5: tab = g_bits5; break;
        default: tab = g_bits6; break;
        }
        v = (int32_t)tab[which * 145 + byte_i];
    }
    if (!(v & (int32_t)g_bit_mask[bit_i]))
        return -1;
    if (byte_i > 0)
        n = Bits_Count(which, byte_i);
    for (i = 0; i <= bit_i; i++)
        if (v & (int32_t)g_bit_mask[i])
            n++;
    return n - 1;
}

/* The rows of each table, indexed by which table and which row. */
/* @0x1003b20c */ extern const uint8_t g_row_sel[22];
/* @0x1012c4a0 */ extern const tv_ref g_row0[];
/* @0x1010b3a0 */ extern const tv_ref g_row1[];
/* @0x100aec90 */ extern const tv_ref g_row2[];
/* @0x10102514 */ extern const tv_ref g_row3[];
/* @0x100c898c */ extern const tv_ref g_row4[];
/* @0x100f8084 */ extern const tv_ref g_row5[];

/*
 * Look for a value in one row of a table.
 *
 * The row's first byte says which of two values to look for, and carries
 * half of the answer in its upper bits; finding the value plus 0x80 rather
 * than the value itself is what makes the search succeed.
 */
/* @0x1003b0d0 */
int32_t TV_THISCALL Stage3_FindRow(Engine *self, int32_t slot, int32_t which,
                                   int32_t lo, int32_t hi, int32_t len)
{
    const uint8_t *tab;
    int32_t off, want, i, b, half;
    int32_t result = -1;
    uint8_t stop = 0, hit = 0;

    i = which - 2;
    if ((uint32_t)i <= 0x15) {
        switch (g_row_sel[i]) {
        case 0: tab = TV_REF(uint8_t, g_row0[which]); break;
        case 1: tab = TV_REF(uint8_t, g_row1[which]); break;
        case 2: tab = TV_REF(uint8_t, g_row2[which]); break;
        case 3: tab = TV_REF(uint8_t, g_row3[which]); break;
        case 4: tab = TV_REF(uint8_t, g_row4[which]); break;
        default: tab = TV_REF(uint8_t, g_row5[which]); break;
        }
    } else {
        /* the original falls back on its own "this" here, which cannot be a
         * table; the callers never ask for a row it does not have */
        tab = (const uint8_t *)self;
    }

    if (slot == 0)
        return result;
    off = ((slot & 0xff) - 1) * (len + 1);
    b = (int32_t)tab[off];
    half = b >> 1;
    want = (b & 1) ? (int32_t)(int8_t)hi : (int32_t)(int8_t)lo;

    for (i = 1; len + 1 > i; i++) {
        if (stop || hit)
            continue;
        b = (int32_t)tab[off + i];
        if (b == want) {
            stop = 1;
        } else if (b == want + 0x80) {
            hit = 1;
            result = Bits_Count(which, 0x91) + half;
        }
    }
    return result;
}

/* Which of seven formant sets a phoneme's nasal pole uses. */
/* @0x100490b0 */ extern const uint8_t g_pole_sel[68];

/*
 * The formants of a nasal murmur.
 *
 * A nasal is voiced through a closed mouth, so its formants come from the
 * nose rather than from where the tongue is; there are seven sets of them,
 * chosen by which nasal it is and what it is next to.
 */
/* @0x10048ef0 */
void TV_THISCALL Stage3_NasalPole(Engine *self, int32_t *out)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t i = px3(st->ctl->value) - 0x34;

    switch (((uint32_t)i <= 0x43) ? g_pole_sel[i] : 6) {
    case 0:
        out[1] = 0x731; out[2] = 0x99e; out[3] = 0xd42;
        out[4] = 0x44;  out[5] = 0x198; out[6] = 0xbc;  out[0] = 0x108;
        break;
    case 1:
        out[1] = 0x646; out[2] = 0x9e6; out[3] = 0xd8c;
        out[4] = 0x4b;  out[5] = 0x198; out[6] = 0x119; out[0] = 0x128;
        break;
    case 2:
        out[1] = 0x66e; out[2] = 0x9a7; out[3] = 0xd6c;
        out[4] = 0x80;  out[5] = 0x4a;  out[6] = 0x37;  out[0] = 0x144;
        break;
    case 3:
        out[1] = 0x423; out[2] = 0xa95; out[3] = 0xcd9;
        out[4] = 0x8d;  out[5] = 0xc4;  out[6] = 0x5e;  out[0] = 0x162;
        break;
    case 4:
        out[1] = 0x5d1; out[2] = 0x96c; out[3] = 0xbe2;
        out[4] = 0x95;  out[5] = 0x4f;  out[6] = 0x3c;  out[0] = 0x17f;
        break;
    case 5:
        out[1] = 0x5b5; out[2] = 0x9fa; out[3] = 0xdc1;
        out[4] = 0xb2;  out[5] = 0x9c;  out[6] = 0x50;  out[0] = 0x183;
        break;
    default:
        out[1] = 0x5b3; out[2] = 0x9d5; out[3] = 0xd19;
        out[4] = 0x7d;  out[5] = 0xb9;  out[6] = 0x87;  out[0] = 0x154;
        break;
    }
}

/*
 * Write one parameter's travel across a phoneme that begins with a burst.
 *
 * The burst itself (s3_1e38 frames of it) is laid down first as its own
 * decay, then a single frame pins the value the phoneme proper starts from,
 * and what is left of the phoneme is filled with one or two straight ramps --
 * one if the caller only wants a single move, two when the parameter has to
 * go somewhere and then come back.
 */
/* @0x1003b290 */
void TV_THISCALL Stage3_Sweep(Engine *self, int32_t v1, int32_t v2,
                              int32_t v3, int32_t v4, int32_t unused,
                              int32_t param, int32_t one_ramp)
{
    StageCtx *st = &self->stage_ctx[3];
    uint8_t *buf;
    int32_t from = 0;    /* the original leaves this from the stack frame */
    int32_t burst, shape, pos, n, k;
    uint8_t c;
    (void)unused;

    switch (param) {
    case 9:  from = self->s3_param_def[param] >> 2; break;
    case 10: from = (self->s3_param_def[param] - 0x1f4) >> 3; break;
    case 11: from = self->s3_param_def[param] >> 4; break;
    case 12: from = self->s3_param_def[param] >> 4; break;
    default: break;
    }

    burst = self->s3_1e38;
    if (burst > 0 && burst != 0x7f) {
        shape = (burst > 0x14) ? 0x14 : burst;
        c = st->cur->value;
        if (param == 9 && !((c == 'P' || c == 'T' || c == 'K') &&
                            (int32_t)(uint8_t)v3 != from)) {
            /* a stop's own burst holds one value throughout */
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, burst, (uint8_t)v3, (uint8_t)v3);
        } else {
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, burst, (uint8_t)from, (uint8_t)v3);
        }
        pos = self->trk_wr[param] + self->s3_1e38;
        if (self->s3_1e38 == 1) {
            self->trk_wr[param] = pos;
            self->s3_param[param].len -= self->s3_1e38;
        }
    } else {
        pos = self->trk_wr[param];
    }

    buf = self->trk_buf[param];
    self->s3_runlen = (int32_t)st->ctl->arg;
    if (self->s3_1e38 != 0 && self->s3_1e38 != 0x7f)
        self->s3_runlen -= self->s3_1e38;

    Track_Decay(self, buf, pos, 1, 1, (uint8_t)v3, (uint8_t)v3);
    pos++;

    n = v2 - 1;
    if (n < 1)
        n = 1;

    if ((uint8_t)one_ramp != 0) {
        k = self->s3_runlen - 1;
        if (k > 0x14)
            k = 0x14;
        Track_RampTo(buf, pos, k, (uint8_t)v3, (uint8_t)v4);
        return;
    }

    if (n > 0) {
        k = (n > 0x14) ? 0x14 : n;
        Track_RampTo(buf, pos, k, (uint8_t)v3, (uint8_t)v1);
        pos += k;
    }
    k = self->s3_runlen - n - 1;
    if (k > 0) {
        if (k > 0x14)
            k = 0x14;
        Track_RampTo(buf, pos, k, (uint8_t)v1, (uint8_t)v4);
    }
}

/*
 * Work out where one parameter has to be at three points across a phoneme,
 * then hand those to Stage3_Sweep.
 *
 * The record holds three (how, value) pairs: "how" says whether the value is
 * absolute, an offset from the parameter's default, or an offset from
 * whichever of the default and the current target is smaller.
 */
/* @0x100504c0 */
void TV_THISCALL Stage3_Glide(Engine *self, int32_t param,
                              tv_ref recs, int32_t idx)
{
    StageCtx *st = &self->stage_ctx[3];
    const S3Param *r = &TV_REF(S3Param, recs)[idx];
    const int32_t *how = &r->mode;    /* mode, shape_in, shape_out */
    const int32_t *val = &r->lead;    /* lead, start, target */
    int32_t dw[3];
    /* the original leaves these from the stack frame when the parameter is
     * not one of the four formants */
    uint8_t b[3] = { 0, 0, 0 };
    int32_t i, v, w, n;

    for (i = 0; i < 3; i++) {
        if (how[i] == 1) {
            if (i == 2)
                dw[i] = val[i] + self->s3_param[param].target;
            else
                dw[i] = val[i] + self->s3_param_def[param];
        } else if (how[i] == 2) {
            v = self->s3_param[param].target;
            w = self->s3_param_def[param];
            if (v >= w)
                v = w;
            dw[i] = v + val[i];
            if (param == 9 && dw[i] < 0xc8)
                dw[i] = 0xcc;
        } else {
            dw[i] = val[i];
        }
    }

    n = (int32_t)((((uint32_t)((int32_t)st->ctl->arg * r->len)) + 0x32) /
                  100u);

    switch (param) {
    case 9:
        for (i = 0; i < 3; i++)
            b[i] = (uint8_t)(dw[i] >> 2);
        break;
    case 10:
        for (i = 0; i < 3; i++)
            b[i] = (uint8_t)((dw[i] - 0x1f4) >> 3);
        break;
    case 11:
        for (i = 0; i < 3; i++) {
            if (dw[i] > 0xb86)
                dw[i] = 0xb86;
            b[i] = (uint8_t)(dw[i] >> 4);
        }
        break;
    case 12:
        for (i = 0; i < 3; i++)
            b[i] = (uint8_t)(dw[i] >> 4);
        break;
    default:
        break;
    }

    Stage3_Sweep(self, (int32_t)b[1], n, (int32_t)b[0], (int32_t)b[2], 0,
                 param, (dw[1] == 0) ? 1 : 0);

    self->s3_param[param].start = dw[0];
    self->s3_param[param].lead = dw[0];
}

/*
 * The glide tables.
 *
 * Each is a list of records -- one per way a formant can travel -- and the
 * lists are chosen by what the glide is between.  The "pair" tables say
 * which record a given pair of phonemes calls for.
 */
/* @0x100d0768 */ extern const tv_ref g_glide_r_vowel;
/* @0x100d076c */ extern const tv_ref g_glide_r_edge;
/* @0x100d0770 */ extern const tv_ref g_glide_any;
/* @0x100d0fc8 */ extern const tv_ref g_glide_y;
/* @0x100d15d8 */ extern const tv_ref g_glide_l;
/* @0x100d0778 */ extern const tv_ref g_glide_r_target;
/* @0x100d0f58 */ extern const tv_ref g_pair_y_tab[];
/* @0x100d0fa8 */ extern const int32_t g_pair_y_count[];
/* @0x100d05f0 */ extern const tv_ref g_pair_tab[];
/* @0x100d0708 */ extern const int32_t g_pair_count[];
/* @0x100d1568 */ extern const tv_ref g_pair_l_tab[];
/* @0x100d15b8 */ extern const int32_t g_pair_l_count[];

/*
 * Where the formants travel across a glide.
 *
 * "R", "L" and "Y" bend the formants further than anything else in the
 * language, and how far depends on both the phoneme before and the one
 * after, so the targets come from tables looked up by that pair rather than
 * from the phoneme's own class.  With "setup" clear the travel itself is
 * laid into the tracks; with it set only the end points are wanted.
 */
/* @0x1004ff20 */
void TV_THISCALL Stage3_Formants(Engine *self, int32_t setup)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    const tv_ref *recs;
    const S3Param *rec;
    const tv_ref *tgt;
    const tv_ref *pairs;
    const int32_t *counts;
    int32_t pick[4];
    uint8_t c_ctl = ctl->value;
    uint8_t c_cur = st->cur->value;
    uint8_t c_scan = st->scan->value;
    uint8_t a_cur, c;
    int32_t i, v, k;

    if ((uint8_t)setup != 0) {
        c = ctl->next->value;
        if ((c == '%' || c == '&') && ctl->next->next->value == 'R') {
            /* an "R" that opens the next word */
            tgt = TV_REF(tv_ref, g_glide_r_target);
            self->s3_param[9].target =
                TV_REF(int32_t, tgt[0])[(c_ctl == 'K') ? 0 : 1];

            if (c_ctl == 'x')      k = 0;
            else if (c_ctl == 'X') k = 1;
            else if (c_ctl == 'K') k = 2;
            else if (c_ctl == 'j' || c_ctl == 'l') k = 3;
            else if (c_ctl == 'P' || c_ctl == 'S') k = 4;
            else k = (c_ctl == 's') ? 5 : 6;
            self->s3_param[10].target = TV_REF(int32_t, tgt[1])[k];

            if (c_ctl == 'G' || c_ctl == 'J' || c_ctl == 'Z' ||
                c_ctl == 'z')
                k = 0;
            else if (c_ctl == 'P') k = 1;
            else if (c_ctl == 'K') k = 2;
            else if (c_ctl == 'X' || c_ctl == 'S') k = 3;
            else k = 4;
            self->s3_param[11].target = TV_REF(int32_t, tgt[2])[k];

            if (c_ctl == '~') k = 0;
            else if (c_ctl == 'R' || c_ctl == 'z') k = 1;
            else k = (c_ctl == 'P') ? 2 : 3;
            self->s3_param[12].target = TV_REF(int32_t, tgt[3])[k];
            return;
        }

        /* otherwise take the end points straight out of the pair table */
        if (c_ctl == 'z' && c_cur == 'J')
            c_ctl = 'D';
        else if (c_ctl == 's' && c_cur == 'C')
            c_ctl = 'T';
        c = ctl->next->next->value;
        if (ctl->next->value == 'L') {
            recs = TV_REF(tv_ref, g_glide_l);
            counts = g_pair_l_count;
            pairs = g_pair_l_tab;
        } else {
            if (c == 'l')
                c = '|';
            recs = TV_REF(tv_ref, g_glide_any);
            counts = g_pair_count;
            pairs = g_pair_tab;
        }
        Stage3_FindPair(pick, c_ctl, c, counts, pairs);
        for (i = 0; i < 4; i++) {
            rec = &TV_REF(S3Param, recs[i])[pick[i]];
            self->s3_param[9 + i].target = rec->lead;
        }
        return;
    }

    a_cur = (uint8_t)(Phone_Attr(px3(c_cur) | 0x100) & 2);
    if (a_cur != 0 && c_ctl == 'R') {
        /* "R" after a vowel */
        recs = TV_REF(tv_ref, g_glide_r_vowel);
        pick[0] = 0;
        if (c_cur == 'A' || c_cur == 'b' || c_cur == 'E' || c_cur == 'I' ||
            c_cur == 'y') {
            pick[1] = 0;
            self->s3_param[13].target = 0x3c;
            self->s3_param[14].target = 0x64;
            self->s3_param[15].target = 0x9c;
            v = (int32_t)(ctl->arg / 3u);
        } else {
            pick[1] = 1;
            self->s3_param[13].target = 0x5e;
            self->s3_param[14].target = 0x61;
            self->s3_param[15].target = 0x82;
            v = (int32_t)(ctl->arg >> 2);
        }
        self->s3_param[10].shape_in = v;
        if (c_cur == 'g' || c_cur == 'c' || c_cur == 'k' || c_cur == 'r' ||
            c_cur == '3' || c_cur == '4' || c_cur == '@' || c_cur == '|')
            pick[2] = 0;
        else
            pick[2] = 1;
        self->s3_param[9].shape_in = (int32_t)(ctl->arg >> 1);
        self->s3_param[11].shape_in = (int32_t)(ctl->arg >> 1);
        for (i = 0; i < 3; i++)
            Stage3_Glide(self, 9 + i, recs[i], pick[i]);
        goto modes;
    }

    c = ctl->prev->value;
    if ((c == '%' || c == '&') && c_ctl == 'R') {
        /* "R" opening a word */
        if (c_cur == ' ')
            c_cur = self->s3_1fed;
        pick[0] = 0;
        if (c_scan == 'E') {
            pick[1] = 0;
        } else if (c_cur == 'j' || c_cur == 'l' || c_cur == 'K' ||
                   c_scan == 'y' || c_scan == 'c' || c_scan == 'g' ||
                   c_scan == '3') {
            pick[1] = 1;
        } else if (c_cur == 'x') {
            pick[1] = 2;
        } else if (c_cur == 'P') {
            pick[1] = 3;
        } else {
            pick[1] = 4;
        }
        if (c_cur == 'G' || c_cur == 'J' || c_cur == 'Z')
            pick[2] = 0;
        else if (c_cur == 'P')
            pick[2] = 1;
        else if (c_cur == 'V')
            pick[2] = 2;
        else
            pick[2] = 3;
        if (c_cur == '~')
            pick[3] = 0;
        else if (c_cur == 'R')
            pick[3] = 1;
        else
            pick[3] = (c_cur == 'P') ? 2 : 3;
        recs = TV_REF(tv_ref, g_glide_r_edge);
        for (i = 0; i < 4; i++)
            Stage3_Glide(self, 9 + i, recs[i], pick[i]);
        goto ramp;
    }

    if (a_cur != 0 && c_ctl == 'Y') {
        Stage3_FindPair(pick, c_cur, c_scan, g_pair_y_count, g_pair_y_tab);
        recs = TV_REF(tv_ref, g_glide_y);
        for (i = 0; i < 4; i++)
            Stage3_Glide(self, 9 + i, recs[i], pick[i]);
        goto modes;
    }

    if (c_cur == ' ')
        c_cur = self->s3_1fed;
    if (c_cur == 'z' && self->s3_1fed == 'J')
        c_cur = 'D';
    else if (c_cur == 's' && self->s3_1fed == 'C')
        c_cur = 'T';
    if (c_ctl == 'L') {
        counts = g_pair_l_count;
        pairs = g_pair_l_tab;
    } else {
        counts = g_pair_count;
        pairs = g_pair_tab;
        if (c_scan == 'l')
            c_scan = '|';
    }
    Stage3_FindPair(pick, c_cur, c_scan, counts, pairs);
    recs = (c_ctl == 'L') ? TV_REF(tv_ref, g_glide_l) : TV_REF(tv_ref, g_glide_any);
    for (i = 0; i < 4; i++)
        Stage3_Glide(self, 9 + i, recs[i], pick[i]);

ramp:
    self->s3_param[12].mode = 1;

modes:
    for (i = 9; i < 12; i++)
        self->s3_param[i].mode = 1;
    for (i = 13; i < 16; i++) {
        self->s3_param[i].mode = 0x14;
        v = (int32_t)(st->cur->arg >> 2);
        if ((uint32_t)v <= 2u)
            v = 2;
        self->s3_param[i].shape_in = v;
        v = (int32_t)(st->ctl->arg >> 2);
        if ((uint32_t)v <= 2u)
            v = 2;
        self->s3_param[i].shape_out = v;
    }
}

/*
 * Fill in the voicing across a closure.
 *
 * A stop closes the mouth, so for the length of the closure there is nothing
 * for the formants to do: the amplitudes are simply written flat.  How loud
 * the voicing stays through it, and for how many frames, is what most of
 * this works out -- a voiceless stop goes silent, a voiced one keeps a low
 * buzz, and "K" keeps more of it than the others.
 */
/* @0x10050740 */
void TV_THISCALL Stage3_Voiced(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    int32_t v, n, len;
    /* the original reads this slot before writing it; it only matters for a
     * glottal stop next to something that is not a stop */
    int32_t spare = 0;

    if (!(cur->flags & 0x20u)) {
        self->s3_1e58 -= 3;
        if (self->s3_1e58 < 0)
            self->s3_1e58 = 0;
    }

    if (Phone_Attr(c_cur) & 0x20) {
        if (st->p_38 & 0x10)
            self->s3_1e38 = 2;
        if ((Phone_Attr(c_cur | 0x100) & 4) &&
            (Phone_Attr(c_ctl | 0x100) & 8))
            self->s3_1e58 += 9;

        if (c_cur == 'P' || c_cur == 'T' || c_cur == 'K') {
            v = self->s3_param_def[2];
            if (!(c_cur == 'P' && c_ctl == 'y'))
                v -= (c_ctl == 'p') ? 6 : 2;
            self->s3_1e58 = v;
            if (self->s3_1e58 < 0)
                self->s3_1e58 = 0;
        }

        if (c_ctl == 'p') {
            v = (c_cur == 'P' || c_cur == 'T' || c_cur == 'K') ? 0 : spare;
            if (v > 0) {
                Track_Fill(self->trk_buf[1], self->trk_wr[1],
                           self->s3_1e38, (uint8_t)v);
                self->trk_wr[1] += self->s3_1e38;
                self->s3_param[1].len -= self->s3_1e38;
            }
        }

        len = (int32_t)ctl->arg - 3;
        if ((uint32_t)self->s3_1e38 > (uint32_t)len)
            self->s3_1e38 = (len != 0) ? len : 1;

        if (((cur->flags & 0x20u) &&
             ((Phone_Attr(c_ctl) & 1) ||
              g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]] == 4)) ||
            (c_cur == 'K' &&
             (c_ctl == 'r' || c_ctl == 'g' || c_ctl == 'c'))) {
            if (c_cur == 'P' || c_cur == 'T' || c_cur == 'K') {
                v = self->s3_param_def[1] - 2;
                if (v < 0)
                    v = 0;
                if (c_cur != 'K' && Phone_IsVowel((uint8_t)c_ctl) &&
                    (ctl->flags & 0x20u))
                    v = 0;
                if (c_cur == 'K') {
                    if (c_ctl == 'i')
                        v = self->s3_param_def[1];
                    if (c_ctl == 'b' || c_ctl == 'u' || c_ctl == 'U' ||
                        c_ctl == 'L')
                        v = self->s3_param_def[1];
                    if (c_ctl == 'O')
                        v = 0x37;
                }
            } else {
                v = 0x3c;
            }

            Track_Fill(self->trk_buf[1], self->trk_wr[1], self->s3_1e38,
                       (uint8_t)v);
            n = self->s3_1e38;
            self->trk_wr[1] += n;
            self->s3_param[1].len -= n;

            v = (c_cur == 'K') ? n : n - 1;
            if (c_cur == 'P')
                v = (c_ctl == 'L') ? n - 3 : n - 1;
            if (v < 0) {
                v = n - 1;
                if (v < 0)
                    v = 0;
            }
            if (v >= self->s3_param[17].len)
                v = self->s3_param[17].len - 1;

            Track_Fill(self->trk_buf[17], self->trk_wr[17], v, 0);
            self->trk_wr[17] += v;
            self->s3_param[17].len -= v;
            self->s3_param_def[17] = self->s3_1fb8;
        }
        self->trk_rd[2] = self->trk_wr[2];
    } else {
        self->s3_1e38 = 1;
        if (!(Phone_Attr(c_ctl) & 1))
            self->s3_1e38 = self->s3_param[2].len / 2;
        if (!((c_ctl == 'R' || (Phone_Attr(c_ctl | 0x100) & 2)) &&
              c_cur == 'X'))
            Track_Nudge(self, self->trk_buf[2], 2, self->trk_wr[2], 3,
                        self->trk_wr[2] - self->trk_rd[2],
                        self->s3_1e58 - self->s3_param_def[2]);
    }

    if (self->s3_1e38 > self->s3_param[2].len)
        self->s3_1e38 = self->s3_param[2].len;
    if (self->s3_1e38 > self->s3_param[0].len)
        self->s3_1e38 = self->s3_param[0].len;

    n = self->s3_1e38;
    if (n <= 0)
        return;

    if (c_ctl == 'R' && (c_cur == 'z' || c_cur == 'V')) {
        Track_Fill(self->trk_buf[0], self->trk_wr[0], n,
                   (uint8_t)self->s3_param_def[0]);
    } else if (c_ctl == 'R' && c_cur == 'X') {
        if (n > 1) {
            n--;
            Track_Fill(self->trk_buf[0], self->trk_wr[0], n, 0);
        }
        Track_Fill(self->trk_buf[0], self->trk_wr[0] + n, 1, 0x39);
        self->s3_param_def[0] = 0x39;
    } else if (Phone_IsVowel((uint8_t)c_ctl) && self->s3_1e38 == 1 &&
               !(Phone_Attr(c_cur) & 0x20)) {
        Track_Fill(self->trk_buf[0], self->trk_wr[0], self->s3_1e38, 0x30);
        self->s3_param_def[0] = 0x30;
    } else {
        Track_Fill(self->trk_buf[0], self->trk_wr[0], self->s3_1e38, 0);
        self->s3_param_def[0] = 0;
    }

    if ((c_ctl == 'R' || (Phone_Attr(c_ctl | 0x100) & 2)) && c_cur == 'X') {
        Track_Fill(self->trk_buf[2], self->trk_wr[2], self->s3_1e38,
                   (uint8_t)self->s3_param_def[2]);
        Track_Fill(self->trk_buf[1], self->trk_wr[1], self->s3_1e38,
                   (uint8_t)self->s3_param_def[1]);
        self->trk_wr[1] += self->s3_1e38;
        self->s3_param[1].len -= self->s3_1e38;
    } else {
        Track_Fill(self->trk_buf[2], self->trk_wr[2], self->s3_1e38,
                   (uint8_t)self->s3_1e58);
    }

    self->s3_param[0].mode = 6;
    self->s3_param_def[2] = self->s3_1e58;
    n = self->s3_1e38;
    self->s3_param[2].len -= n;
    self->trk_wr[2] += n;
    self->s3_param[0].len -= n;
    self->trk_wr[0] += n;
}

/*
 * Coarticulation: let the phonemes either side pull this one about.
 *
 * Everything so far has treated the phoneme on its own.  This is the pass
 * that does not: it looks at what came before and what comes next and
 * adjusts how long each parameter has to travel, which end value it aims
 * for, and in a few named cases substitutes values outright -- the nasals,
 * the glottal stop before a flap, "M" before a vowel.
 */
/* @0x1004f710 */
void TV_THISCALL Stage3_Coarticulate(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *n;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    int32_t c_scan = px3(st->scan->value);
    int32_t back;
    uint8_t a_ctl, a_cur, c;
    int32_t i, v;

    n = cur->prev;
    back = px3(n->value);
    if (Phone_Attr(back) & 8) {
        n = n->prev;
        back = (n != NULL) ? px3(n->value) : 0x20;
    }

    if (c_ctl == 'M' && c_cur == 'a') {
        if (Phone_Attr(c_scan | 0x100) & 0x10) {
            for (i = 10; i < 13; i++)
                self->s3_param[i].shape_in = 2;
            for (i = 10; i < 13; i++)
                self->s3_param[i].shape_out = 3;
        } else if (c_scan == 'p') {
            for (i = 10; i < 12; i++)
                self->s3_param[i].shape_in = 3;
            for (i = 11; i < 13; i++)
                self->s3_param[i].shape_out = 3;
            self->s3_param[10].shape_out = 4;
        }
    }

    if (self->s3_2034 != 0 && !(Phone_Attr(c_ctl | 0x100) & 2)) {
        /* the phoneme before was cut short: start from where it ended */
        for (i = 9; i < 13; i++) {
            self->s3_param[i].mode &= ~1;
            if (!(Phone_Attr(c_ctl) & 0x10))
                self->s3_param[i].start = self->s3_param_def[i];
        }
        if (c_ctl == 'L' && !Phone_IsVowel((uint8_t)c_cur))
            self->s3_param[9].mode = 4;
        self->s3_2034 = 0;
    }

    if (Phone_IsVowel((uint8_t)back) &&
        !(cur->value == 'Y' && (Phone_Attr(back | 0x180) & 0x40))) {
        for (i = 9; i < 13; i++)
            if ((uint32_t)self->s3_param[i].shape_in > cur->arg)
                self->s3_param[i].shape_in = (int32_t)cur->arg;
    }

    a_ctl = Phone_Attr(c_ctl | 0x100);
    if ((a_ctl & 1) && (Phone_Attr(c_ctl) & 0x20)) {
        for (i = 13; i < 16; i++)
            self->s3_param[i].shape_out = self->s3_param[i].len - 1;
    }

    if (c_ctl == 'n' || c_cur == 'n') {
        self->s3_param[16].mode = 0x14;
        self->s3_param[16].shape_out = 3;
        self->s3_param[16].shape_in = 3;
        if (c_cur == 'n' && c_ctl != 'R') {
            for (i = 9; i < 13; i++) {
                self->s3_param[i].mode |= 1;
                self->s3_param[i].shape_in = (int32_t)(cur->arg / 3u);
            }
            for (i = 13; i < 16; i++) {
                self->s3_param[i].mode |= 1;
                self->s3_param[i].shape_in = (int32_t)(cur->arg / 3u);
            }
        }
    }

    a_cur = Phone_Attr(c_cur | 0x100);
    if ((a_cur & 2) && c_cur != 'p') {
        /* into a vowel: the bandwidths follow the vowel's own shape */
        for (i = 13; i < 16; i++) {
            self->s3_param[i].mode |= 1;
            self->s3_param[i].shape_in = (int32_t)(cur->arg >> 2);
            self->s3_param[i].lead = (self->s3_param[i].mode == 5)
                                     ? self->s3_param[i].target
                                     : self->s3_param[i].start;
            if (a_ctl & 2) {
                if (Phone_Attr(c_cur | 0x80) & 0x10) {
                    v = 1;
                } else {
                    v = (int32_t)(cur->arg / 3u);
                    if (v <= 1)
                        v = 2;
                }
                self->s3_param[i].shape_in = v;
                if (Phone_Attr(c_ctl | 0x80) & 0x10) {
                    v = 0;
                } else {
                    v = (int32_t)(ctl->arg / 3u);
                    if (v <= 1)
                        v = 2;
                }
                self->s3_param[i].shape_out = v;
                self->s3_param[i].mode = 0x10;
                if (self->s3_2034 != 0) {
                    self->trk_wr[i] -= (int32_t)st->ctl->arg;
                    self->s3_param[i].len = (int32_t)st->ctl->arg;
                }
            } else {
                v = (int32_t)(cur->arg / 3u);
                if (v <= 1)
                    v = 2;
                self->s3_param[i].shape_in = v;
                v = (int32_t)(ctl->arg / 3u);
                if (v <= 1)
                    v = 2;
                self->s3_param[i].shape_out = v;
                self->s3_param[i].mode = 0x14;
            }
        }

        if (c_ctl == ' ') {
            self->s3_param[13].start = 0x12c;
            self->s3_param[13].lead = 0x12c;
            self->s3_param[14].start = 0xbc;
            self->s3_param[14].lead = 0xbc;
            self->s3_param[15].start = 0xc2;
            self->s3_param[15].lead = 0xc2;
        }
        if (c_ctl == 'j' && Phone_IsVowel(st->scan->value) &&
            !(Phone_Attr(px3(cur->value) | 0x100) & 2))
            self->s3_param[9].target = self->s3_param_def[9];
    } else if ((a_ctl & 2) && c_ctl != 'p') {
        /* out of a vowel */
        for (i = 13; i < 16; i++) {
            v = (int32_t)(cur->arg / 3u);
            if (v <= 1)
                v = 2;
            self->s3_param[i].shape_in = v;
            v = ((int32_t)(ctl->arg / 3u) > 1) ? 3 : 2;
            self->s3_param[i].shape_out = v;
            self->s3_param[i].mode = 0x10;
            if (self->s3_2034 != 0) {
                self->trk_wr[i] -= (int32_t)st->ctl->arg;
                self->s3_param[i].len = (int32_t)st->ctl->arg;
            }
        }
    }

    if (c_cur == 'R') {
        c = cur->prev->value;
        if ((Phone_Attr(px3(c) | 0x100) & 2) && !Phone_IsVowel(c)) {
            for (i = 9; i < 13; i++)
                self->s3_param[i].mode &= 6;
        }
    }

    if (c_ctl == 'M' && (c_cur == 'E' || (a_cur & 2))) {
        self->s3_param[9].mode = 8;
        self->s3_param[9].shape_out = (int32_t)(ctl->arg >> 1);
        self->s3_param[9].start = self->s3_param_def[9];
        self->s3_param[9].target = self->s3_param_def[9] - 0x32;
    }
    if (c_ctl == 'M' && (Phone_Attr(c_cur | 0x180) & 0x40) &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2)) {
        self->s3_param[10].mode = 0x14;
        self->s3_param[10].shape_out = (int32_t)(ctl->arg >> 1);
        self->s3_param[10].shape_in = (int32_t)(cur->arg >> 2);
    }

    if (self->s3_param[9].target > 0x2bc && (a_ctl & 2))
        self->s3_param[0].target -= 3;

    if (ctl->value == 'p' && cur->value == '~') {
        /* a glottal stop closing a flap */
        self->s3_param[0].target = 0x37;
        self->s3_param[3].mode = 4;
        self->s3_param[4].target = 0;
        self->s3_param[4].mode = 4;
        self->s3_param[5].target = 0;
        self->s3_param[5].mode = 4;
        self->s3_param[6].target = 0x37;
        self->s3_param[6].mode = 4;
        self->s3_param[7].target = 0;
        self->s3_param[3].target = 0x39;
        self->s3_param[9].target = 0xfa;
        self->s3_param[10].target = 0x620;
        self->s3_param[10].mode = 7;
        self->s3_param[10].shape_out = 2;
        self->s3_param[7].mode = 4;
        self->s3_param[10].start = 0x6a4;
        self->s3_param[11].target = 0x8a0;
        self->s3_param[12].target = 0xe10;
        Track_Fill(self->trk_buf[1], self->trk_wr[1], 1, 0x3a);
        self->trk_wr[1]++;
        self->s3_param[1].len--;
        Track_Fill(self->trk_buf[0], self->trk_wr[0], 1, 0x33);
        self->trk_wr[0]++;
        self->s3_param[0].len--;
    }

    if (c_ctl == '~') {
        if ((a_cur & 2) && st->scan->value == 'p') {
            self->s3_param[9].target = 0xfa;
            self->s3_param[10].target = 0x4ac;
            self->s3_param[11].target = 0x8f0;
        }
        if (c_cur == 'i') {
            self->s3_param[10].shape_in = 3;
            self->s3_param[10].lead = 0x5dc;
            self->s3_param[10].shape_out = 3;
            self->s3_param[10].start = 0x5dc;
            self->s3_param[10].mode = 7;
        }
    }
    if (c_ctl == 'a')
        self->s3_param[0].target = 0x3c;
}

/* Which per-phoneme routine each consonant calls for. */
/* @0x10050cdc */ extern const uint8_t g_op16_sel[0x39];
/* @0x10001d60 */ extern const uint8_t g_op16_r_sel[0x2d];
/* @0x100023ec */ extern const uint8_t g_op16_l_sel[0x39];
/* @0x100029b8 */ extern const uint8_t g_op16_zh_sel[0x5d];

/*
 * Where the tongue is for the vowel coming up.
 *
 * Seven groups, roughly front-to-back and close-to-open, which the routines
 * below use to pick how far the formants have to travel out of the
 * consonant.  A consonant of its own gets group 6, and anything left over
 * that is a glide gets 7.
 */
/* @0x10001000 */
void TV_THISCALL Stage3_VowelGroup(Engine *self)
{
    int32_t c = px3(self->stage_ctx[3].cur->value);

    switch (c) {
    case 'E': case 'A':
        self->s3_2010 = 1;
        break;
    case 'y': case 'I': case '|': case 'i': case 'e': case 'a':
        self->s3_2010 = 2;
        break;
    case 'U': case 'b': case 'f': case 'O':
        self->s3_2010 = 3;
        break;
    case 'u': case 'v': case 'w': case 'o': case '@':
        self->s3_2010 = 4;
        break;
    case '4': case 'k': case '3': case 'r': case 'g': case 'c': case '5':
        self->s3_2010 = 5;
        break;
    case 'M': case 'B': case 'V': case 'P': case 'F': case 'W':
    case 'L': case 'j': case 'R': case 'H': case ' ':
        self->s3_2010 = 6;
        break;
    default:
        if ((Phone_Attr(c | 0x80) & 0x20) && c != 'd')
            self->s3_2010 = 7;
        break;
    }
}

/* "B" before "LS". */
/* @0x10001170 */
void TV_THISCALL Stage3_Op16_B(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];

    if (st->scan->value == 'L' && st->cur->value == 'S')
        self->s3_param[3].target = 0x46;
}

/* "D": how much of the burst survives into what follows. */
/* @0x100011a0 */
void TV_THISCALL Stage3_Op16_D(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *scan = st->scan;
    int32_t c_cur = px3(st->cur->value);
    int32_t c_scan = px3(scan->value);
    int32_t k;

    if (Phone_Attr(c_scan | 0x80) & 0x20) {
        if (c_cur == 'j') {
            if ((Phone_Attr(c_scan | 0x100) & 1) &&
                !(Phone_Attr(c_scan) & 0x10)) {
                self->s3_1e5c[1] = 0x3c;
            } else if ((Phone_Attr(c_scan) & 0x40) && c_scan != 'Z') {
                self->s3_1e5c[1] = 0x3c;
            } else if (c_scan == ' ') {
                self->s3_1e5c[1] = 0x3c;
            } else if (c_scan != 'Z') {
                self->s3_1e5c[0] = 0x39;
            }
        } else if (c_cur == ' ' && (Phone_Attr(c_scan) & 0x10)) {
            self->s3_1e5c[1] = 0x37;
        } else if (c_scan == 'n') {
            self->s3_1e5c[0] = 0x32;
        } else if (c_scan == 'Z') {
            self->s3_1e5c[1] = 0x39;
        } else {
            self->s3_1e5c[0] = 0x7f;
            if (Phone_Attr(c_scan) & 0x40) {
                self->s3_1e5c[1] = 0x36;
                if (Phone_Attr(c_cur) & 0x10)
                    self->s3_1e5c[1] = 0x33;
            } else {
                self->s3_1e5c[1] = 0x39;
                if (c_cur == ' ')
                    self->s3_1e5c[1] = 0x37;
            }
        }
    } else if (Phone_Attr(c_scan | 0x100) & 2) {
        if (c_scan == 'p') {
            if (c_cur == 'N') {
                self->s3_param[8].target = 0;
                self->s3_1e5c[1] = 0x37;
                self->s3_param[9].target = 0xfa;
                self->s3_param[10].target = 0x72a;
                self->s3_param[11].target = 0xa3e;
                self->s3_param[12].target = 0xd8d;
                self->s3_param[13].target = 0x8f;
                self->s3_param[14].target = 0x16e;
                self->s3_param[15].target = 0xa6;
            }
        } else {
            k = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_scan]];
            if (k == 0) {
                self->s3_param[8].target = 0x3e;
            } else if (k == 1) {
                if (c_cur == 'S')
                    self->s3_param[8].target = 0x37;
            } else if (k == 3) {
                self->s3_param[8].target = 0x41;
                if (c_scan == '3' &&
                    ((scan->flags & 0x20u) || c_cur != 'j'))
                    self->s3_param[8].target = 0x32;
            }

            if (!(scan->flags & 0x20u)) {
                if (c_cur == 'j') {
                    self->s3_1e5c[0] = 0x35;
                    self->s3_1e5c[1] = 0x34;
                } else {
                    k = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_scan]];
                    if (k == 1) {
                        self->s3_1e5c[0] = 0x2d;
                        self->s3_1e5c[1] = 0x37;
                    } else if (k == 0) {
                        self->s3_1e5c[0] = 0x7f;
                        if (((Phone_Attr(c_cur | 0x100) & 1) &&
                             !(Phone_Attr(c_cur) & 0x10)) ||
                            (Phone_Attr(c_cur) & 0x40))
                            self->s3_1e5c[1] = 0x37;
                        else
                            self->s3_1e5c[1] = 0x34;
                    }
                }
            } else if (c_cur == 'S') {
                self->s3_1e5c[0] = 0x7f;
                self->s3_1e5c[1] = 0x3a;
                self->s3_1e5c[2] = 0x30;
            } else {
                k = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_scan]];
                if (k == 0) {
                    self->s3_1e5c[0] = 0;
                    self->s3_1e5c[1] = 0x37;
                } else if (k == 1) {
                    self->s3_1e5c[0] = 0x2d;
                    self->s3_1e5c[1] = 0x37;
                }
            }
        }
    }

    if (c_scan == 'R' && c_cur == 'S' && (st->ctl->flags & 0x20u))
        self->s3_1e5c[1] = 0x3c;
}

/* "G": where the closure was made decides where the formants come from. */
/* @0x100014b0 */
void TV_THISCALL Stage3_Op16_G(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    uint8_t c_scan;
    uint8_t a;

    Stage3_VowelGroup(self);
    if (st->cur->value != ' ') {
        self->s3_param_rate[9] = 0x7ed8;
        self->s3_param[9].shape_in = 3;
    }

    c_scan = st->scan->value;
    if (c_scan == 'o') {
        self->s3_param[18].target = 8;
        return;
    }
    a = Phone_Attr(px3(c_scan) | 0x100);
    if (a & 2) {
        self->s3_param[18].target = 0;
        return;
    }
    if (!(a & 1) && (Phone_Attr(px3(c_scan)) & 1)) {
        switch (self->s3_2010) {
        case 1:
            self->s3_param[3].target = 0x3e;
            self->s3_param[4].target = 0x3e;
            self->s3_param[5].target = 0x50;
            self->s3_param[6].target = 0x50;
            self->s3_param[7].target = 0x50;
            return;
        case 2:
            self->s3_param[5].target = 0x50;
            self->s3_param[6].target = 0x41;
            self->s3_param[7].target = 0x3c;
            self->s3_param[10].target = 0x898;
            self->s3_param[11].target = 0x960;
            self->s3_param[3].target = 0x3e;
            self->s3_param[4].target = 0x3e;
            return;
        case 3:
            self->s3_param[3].target = 0x50;
            self->s3_param[4].target = 0x4b;
            self->s3_param[5].target = 0x28;
            self->s3_param[6].target = 0x48;
            self->s3_param[7].target = 0x32;
            self->s3_param[10].target = 0x802;
            self->s3_param[11].target = 0x8ca;
            return;
        case 4:
            self->s3_param[10].target = 0x7ac;
            self->s3_param[11].target = 0x874;
            return;
        case 5:
            self->s3_param[3].target = 0x41;
            self->s3_param[4].target = 0x4b;
            self->s3_param[5].target = 0x28;
            self->s3_param[6].target = 0x48;
            self->s3_param[7].target = 0x32;
            return;
        default:
            return;
        }
    }

    if (self->s3_2010 == 3) {
        self->s3_param[10].target = 0x640;
        self->s3_param[11].target = 0x708;
    } else if (self->s3_2010 == 4 || self->s3_2010 == 5) {
        self->s3_param[10].target = 0x694;
        self->s3_param[11].target = 0x75c;
    }
}

/* "J". */
/* @0x100016a0 */
void TV_THISCALL Stage3_Op16_J(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    uint8_t c_cur;

    if ((st->scan->value == 'z' && st->cur->value == ' ') ||
        !(Phone_Attr(px3(st->cur->value)) & 4))
        self->s3_param[0].target = 0;

    c_cur = st->cur->value;
    if ((Phone_Attr(px3(c_cur)) & 4) || c_cur == ' ') {
        self->s3_1e5c[0] = 0x30;
        self->s3_1e5c[1] = 0x40;
    }
}

/* "M" and "m". */
/* @0x10001710 */
void TV_THISCALL Stage3_Op16_M(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    uint8_t c_scan = st->scan->value;
    int32_t c_cur = px3(st->cur->value);
    int32_t g;

    Stage3_VowelGroup(self);
    if (Phone_IsVowel(c_scan))
        return;

    g = self->s3_2010;
    if (g == 1 || g == 2 || c_cur == 'Y') {
        self->s3_param_rate[10] = 0x4010;
        self->s3_param_rate[11] = 0x6680;
        return;
    }
    self->s3_param_rate[10] = 0x6680;
    self->s3_param_rate[11] = 0x2670;
    if (g == 5 || c_cur == 'R')
        self->s3_param[11].target = 0x6d6;
}

/* "R": the formants it leaves behind depend on what it ran into. */
/* @0x10001840 */
void TV_THISCALL Stage3_Op16_R(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    Node *scan = st->scan;
    uint8_t c_scan = scan->value;
    int32_t i = px3(cur->value) - 0x47;
    int32_t k, v;

    switch (((uint32_t)i <= 0x2c) ? g_op16_r_sel[i] : 5) {
    case 0:
        if (!Phone_IsVowel(c_scan)) {
            self->s3_param[0].target = 0x37;
            self->s3_param[10].target = 0x474;
            self->s3_param[11].target = 0x578;
        }
        break;
    case 1:
        if (!Phone_IsVowel(c_scan)) {
            self->s3_param[1].target = 0x38;
            self->s3_param[2].target = 0x33;
            self->s3_param[3].target = 0x41;
            self->s3_param[5].target = 0x37;
            self->s3_param[10].target = 0x57c;
            self->s3_param[12].target = 0x9f0;
            self->s3_param[0].target = 0;
            self->s3_param[4].target = 0;
            self->s3_param[6].target = 0x3c;
            self->s3_param[7].target = 0x3c;
        }
        break;
    case 2:
        if (c_scan == 'e') {
            self->s3_param[3].target = 0x32;
            self->s3_param[9].target = 0x1ca;
            self->s3_param[10].target = 0x514;
            self->s3_param[11].target = 0x718;
            self->s3_param[12].target = 0xb98;
        }
        if (Phone_Attr(px3(scan->value) | 0x100) & 2) {
            if (!(scan->flags & 0x20u)) {
                self->s3_param[10].target = 0x3e4;
                self->s3_param[11].target = 0x5c0;
            } else {
                self->s3_param[8].target = 0x36;
            }
        }
        break;
    case 3:
        if (c_scan == 'E') {
            self->s3_param[10].target = 0x485;
            self->s3_param[11].target = 0x5d2;
            self->s3_param[12].target = 0xbc2;
            self->s3_param[13].target = 0x36;
            self->s3_param[14].target = 0xf7;
            self->s3_param[15].target = 0xe6;
        }
        break;
    case 4:
        if (cur->prev->value == 'C') {
            self->s3_param[10].target = 0x50c;
            self->s3_param[11].target = 0x6e0;
        }
        break;
    default:
        if (Phone_IsVowel(c_scan))
            break;
        k = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(c_scan)]];
        if (k == 0) {
            self->s3_param[0].target = 0x3c;
        } else if (k == 2) {
            self->s3_param[0].target = 0x3c;
            self->s3_param[10].target = 0x474;
            self->s3_param[11].target = 0x5a0;
        } else {
            v = self->s3_param[10].target;
            v -= Synth_MulQ15(v, 0x2008);
            self->s3_param[10].target = v;
            v = ((int32_t)g_pt_f2[TV_REF(uint8_t, g_phone_class)[px3(scan->value)]] << 3) +
                0x1f4;
            self->s3_param[10].target += Synth_MulQ15(v, 0x2008);
            self->s3_param[11].target = self->s3_param[10].target + 0x190;
        }
        break;
    }

    if (scan->value == 'l') {
        self->s3_param[9].target = 0x178;
        self->s3_param[10].target = 0x408;
        self->s3_param[11].target = 0x687;
        self->s3_param[12].target = 0xac5;
    }
}

/* "W". */
/* @0x10001d90 */
void TV_THISCALL Stage3_Op16_W(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    int32_t c_cur = px3(st->cur->value);

    if (c_cur == 'K') {
        self->s3_param[10].target = 0x320;
        return;
    }
    if (c_cur == ' ' && st->scan->value == 'E')
        self->s3_param[15].target = 0xfa;
}

/* "X". */
/* @0x10001dd0 */
void TV_THISCALL Stage3_Op16_X(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];

    if (Phone_Attr(px3(st->cur->value) | 0x100) & 2) {
        self->s3_param_rate[10] = 0x6680;
        self->s3_param_rate[11] = 0x6680;
    }
    if (st->scan->value == 'R') {
        int32_t len = self->s3_param[1].len;

        self->s3_param[1].lead = 0x19;
        self->s3_param[1].start = 0x19;
        self->s3_param[1].shape_in = 0;
        self->s3_param[1].shape_out = len;
    }
}

/* "Y". */
/* @0x10001e30 */
void TV_THISCALL Stage3_Op16_Y(Engine *self)
{
    if (self->stage_ctx[3].cur->value == 'G') {
        self->s3_param[10].target = 0x7f4;
        self->s3_param[11].target = 0xa20;
    }
}

/* "Z". */
/* @0x10001e60 */
void TV_THISCALL Stage3_Op16_Z(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *scan = st->scan;
    int32_t c_cur = px3(st->cur->value);
    uint8_t c_scan = scan->value;

    if (c_scan == 'I' || c_scan == 'a')
        self->s3_param[1].target = 0x40;
    else if (Phone_Attr(c_cur | 0x100) & 1)
        self->s3_param[1].target = 0x3c;
    else
        self->s3_param[1].target = 0x45;

    self->s3_param[18].shape_out = 3;
    self->s3_param[19].shape_out = 3;

    if (!Phone_IsVowel(c_scan)) {
        if (Phone_Attr(c_cur | 0x100) & 2) {
            self->s3_param[0].target = 0x38;
            self->s3_param[4].target = 0x3c;
            self->s3_param[6].target = 0x43;
            self->s3_param[7].target = 0x3f;
            self->s3_param[9].target = 0x128;
            self->s3_param[18].target = 8;
            self->s3_param[19].target = 0xe;
        } else {
            self->s3_param[0].target = 0;
            self->s3_param[4].target = 0x32;
            self->s3_param[5].target = 0x49;
            self->s3_param[1].shape_out = 3;
            self->s3_param[6].target = 0x46;
            self->s3_param[7].target = 0x46;
            self->s3_param[18].target = 0xf;
            self->s3_param[19].target = 0xf;
            c_scan = scan->value;
            if (!(Phone_Attr(px3(c_scan) | 0x100) & 2) && c_scan != ' ') {
                self->s3_param[17].mode = 7;
                self->s3_param_rate[17] = 0x4010;
                self->s3_param[17].target = 0x50;
            }
        }
    }

    if ((Phone_Attr(px3(scan->value) | 0x100) & 1) &&
        (Phone_Attr(px3(scan->value)) & 4)) {
        self->s3_param[18].target = 8;
        self->s3_param[19].target = 0xf;
    }
}

/* "j" and "l": the two liquids, with a table of targets per vowel. */
/* @0x100020c0 */
void TV_THISCALL Stage3_Op16_L(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    int32_t c_cur = px3(st->cur->value);
    int32_t i, v;

    if (ctl->value == 'j') {
        i = c_cur - 0x41;
        switch (((uint32_t)i <= 0x38) ? g_op16_l_sel[i] : 6) {
        case 0:
            self->s3_param[9].target = 0x1cb;
            self->s3_param[10].target = 0x32a;
            self->s3_param[11].target = 0xa7a;
            self->s3_param[12].target = 0xe25;
            return;
        case 1:
            self->s3_param[9].target = 0x17a;
            self->s3_param[10].target = 0x365;
            self->s3_param[11].target = 0xaa4;
            self->s3_param[12].target = 0xe83;
            return;
        case 2:
            self->s3_param[9].target = 0x190;
            self->s3_param[10].target = 0x369;
            self->s3_param[11].target = 0xa2a;
            self->s3_param[12].target = 0xe04;
            return;
        case 3:
            self->s3_param[9].target = 0x187;
            self->s3_param[10].target = 0x32a;
            self->s3_param[11].target = 0xb97;
            self->s3_param[12].target = 0xf28;
            return;
        case 4:
            self->s3_param[9].target = 0x1a6;
            self->s3_param[10].target = 0x34b;
            self->s3_param[11].target = 0xa77;
            self->s3_param[12].target = 0xea6;
            self->s3_param[13].target = 0x4e;
            self->s3_param[14].target = 0x4f;
            self->s3_param[15].target = 0x6b;
            return;
        case 5:
            if (Phone_Attr(px3(st->scan->value) | 0x100) & 2) {
                self->s3_param[9].target = 0x19a;
                self->s3_param[10].target = 0x496;
                self->s3_param[11].target = 0x96c;
                self->s3_param[12].target = 0xe0c;
                for (i = 9; i < 13; i++) {
                    self->s3_param_rate[i] = 0xcd0;
                    self->s3_param[i].shape_out = 4;
                    self->s3_param[i].shape_in = 2;
                }
                return;
            }
            self->s3_param[9].target = 0x224;
            self->s3_param[10].target = 0x384;
            self->s3_param[11].target = 0xad0;
            self->s3_param[12].target = 0xd10;
            self->s3_param_rate[11] = 0xcd0;
            self->s3_param_rate[10] = 0xcd0;
            self->s3_param_rate[9] = 0xcd0;
            self->s3_param[11].shape_out = (int32_t)ctl->arg;
            return;
        default:
            return;
        }
    }

    if (st->scan->value == ' ') {
        self->s3_param[9].target = 0x214;
        self->s3_param[10].target = 0x316;
        self->s3_param[11].target = 0xa56;
        self->s3_param[12].target = 0xd31;
        for (i = 9; i < 13; i++)
            self->s3_param[i].shape_out = (int32_t)st->ctl->arg;
    } else {
        self->s3_param[9].target = 0x154;
        self->s3_param[10].target = 0x32e;
        self->s3_param[13].target = 0x28;
        self->s3_param[14].target = 0x28;
        self->s3_param[11].target = 0xa60;
        self->s3_param[12].target = 0xd91;
        self->s3_param[15].target = 0x78;
        for (i = 9; i <= 15; i++) {
            v = (int32_t)(55u * st->ctl->arg / 100u);
            self->s3_param[i].shape_out = v;
            self->s3_param[i].shape_in = 3;
            if (i >= 9 && i <= 12)
                self->s3_param_rate[i] = 0xcd0;
        }
    }

    if (st->cur->value == 'R') {
        for (i = 9; i < 13; i++)
            self->s3_param[i].mode = 6;
    }
}

/* "s", and the "C" affricate built on it. */
/* @0x10002430 */
void TV_THISCALL Stage3_Op16_Sh(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    uint8_t c_scan = st->scan->value;
    uint8_t c_back = cur->prev->value;
    int32_t c_cur = px3(cur->value);
    int32_t i, v;

    Stage3_VowelGroup(self);
    if (!Phone_IsVowel(c_scan)) {
        switch (self->s3_2010) {
        case 1:
            self->s3_param[6].target = 0x40;
            self->s3_param[7].target = 0x37;
            self->s3_param[15].target = 0xb8;
            break;
        case 2:
            self->s3_param[7].target = 0x35;
            self->s3_param[11].target = 0x900;
            self->s3_param[12].target = 0xbd0;
            self->s3_param[4].target = 0x41;
            self->s3_param[5].target = 0x41;
            break;
        case 3:
            self->s3_param[7].target = 0x36;
            self->s3_param[10].target = 0x6a4;
            self->s3_param[3].target = 0x3c;
            self->s3_param[4].target = 0x3c;
            self->s3_param[5].target = 0x3c;
            break;
        case 4:
            self->s3_param[5].target = 0x3a;
            self->s3_param[12].target = 0xdb0;
            self->s3_param[6].target = 0x30;
            self->s3_param[7].target = 0x30;
            break;
        case 5:
            self->s3_param[4].target = 0x41;
            self->s3_param[7].target = 0x36;
            self->s3_param[11].target = 0x8d0;
            self->s3_param[12].target = 0xc60;
            break;
        default:
            break;
        }
    }

    v = px3(st->scan->value);
    if (v == '@') {
        self->s3_param[1].mode = 6;
        self->s3_param[1].shape_out += 3;
        self->s3_param[1].target -= 3;
        for (i = 4; i < 8; i++) {
            self->s3_param[i].mode = 6;
            self->s3_param[i].shape_out += 3;
        }
        if (c_cur == 'P') {
            self->s3_param[5].mode = 4;
            self->s3_param[4].mode = 4;
        }
    } else if (v == 'b') {
        self->s3_param[1].shape_out = (int32_t)(st->ctl->arg >> 1);
    }

    if (cur->value != 'C')
        return;
    v = px3(st->scan->value);
    if (v == ' ') {
        self->s3_param[1].target = 0x3c;
        if (!Phone_IsVowel(c_back)) {
            self->s3_param[10].target = 0x9ec;
            self->s3_param[11].target = 0xb70;
            self->s3_param[12].target = 0xea0;
        }
    } else if (v == 'R') {
        self->s3_param[2].target = 0x32;
        self->s3_param[10].target = 0x574;
        self->s3_param[11].target = 0x790;
        self->s3_param[7].target = 0x3c;
        if (st->ctl->flags & 0x20u) {
            self->s3_param[1].target = 0x3c;
            self->s3_param[4].target = 0x42;
        } else {
            self->s3_param[1].target = 0x3a;
        }
    }
}

/* "x". */
/* @0x10002860 */
void TV_THISCALL Stage3_Op16_Hh(Engine *self)
{
    int32_t c_cur = px3(self->stage_ctx[3].cur->value);

    if (c_cur == ' ') {
        self->s3_param[10].target = 0x60e;
        self->s3_param[7].target = 0x32;
        return;
    }
    if (Phone_Attr(c_cur | 0x100) & 2) {
        self->s3_param_rate[11] = 0x6680;
        self->s3_param_rate[10] = 0x6680;
    }
}

/* "z", and the "J" affricate built on it. */
/* @0x100028b0 */
void TV_THISCALL Stage3_Op16_Zh(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    int32_t i;

    if (Phone_Attr(px3(cur->value) | 0x100) & 2) {
        self->s3_param_rate[11] = 0x6680;
        self->s3_param_rate[10] = 0x6680;
    }
    if (cur->value != 'J')
        return;

    i = px3(st->scan->value) - 0x20;
    switch (((uint32_t)i <= 0x5c) ? g_op16_zh_sel[i] : 8) {
    case 0:
        self->s3_param[0].lead = 0x2f;
        self->s3_param[0].shape_out = (int32_t)(st->ctl->arg >> 2);
        return;
    case 5:
        for (i = 9; i < 13; i++)
            self->s3_param[i].target = self->s3_param_def[i];
        /* fall through */
    case 1:
    case 2:
    case 3:
    case 4:
    case 7:
        self->s3_param[1].target = self->s3_param_def[1];
        self->s3_param[2].target = self->s3_param_def[2];
        return;
    case 6:
        if (cur->prev->value == ' ') {
            self->s3_param[9].target = 0xfe;
            self->s3_param[10].target = 0x64d;
            self->s3_param[11].target = 0x828;
            self->s3_param[12].target = 0xaaf;
        }
        return;
    default:
        return;
    }
}

/*
 * The last parameter pass: one routine per consonant.
 *
 * Everything general has been applied by now.  What is left is what each
 * consonant does that nothing else does, so the rules hand off here and this
 * picks the routine by the phoneme that just finished.
 */
/* @0x10050c30 */
void TV_THISCALL Stage3_Op16(Engine *self)
{
    int32_t i = px3(self->stage_ctx[3].ctl->value) - 0x42;

    switch (((uint32_t)i <= 0x38) ? g_op16_sel[i] : 16) {
    case 0:  Stage3_Op16_B(self);  break;
    case 1:  Stage3_Op16_D(self);  break;
    case 2:  Stage3_Op16_G(self);  break;
    case 3:  Stage3_Op16_J(self);  break;
    case 4:  Stage3_Op16_M(self);  break;
    case 5:  Stage3_Op16_R(self);  break;
    case 6:  Stage3_Op16_W(self);  break;
    case 7:  Stage3_Op16_X(self);  break;
    case 8:  Stage3_Op16_Y(self);  break;
    case 9:  Stage3_Op16_Z(self);  break;
    case 10: Stage3_Op16_L(self);  break;
    case 11: Stage3_Op16_L(self);  break;
    case 12: Stage3_Op16_M(self);  break;
    case 13: Stage3_Op16_Sh(self); break;
    case 14: Stage3_Op16_Hh(self); break;
    case 15: Stage3_Op16_Zh(self); break;
    default: break;
    }
}

/*
 * Lay one formant and its bandwidth across a glide.
 *
 * Three pieces go into the track: the frames still owed to the phoneme
 * before, the glide to where this one settles, and the rest of the run.  The
 * matching bandwidth, four parameters along, is written the same way
 * afterwards -- except for the third formant, whose bandwidth is left alone.
 */
/* @0x1003a7c0 */
void TV_THISCALL Stage3_LayGlide(Engine *self, int32_t settle, int32_t hold,
                                 int32_t from, int32_t after,
                                 int32_t bw_settle, int32_t param,
                                 int32_t unused)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    int32_t pos, n, k, half;
    uint8_t bw;
    /* the original reads this slot before writing it; param is always 9..12
     * here, so the default arm never runs */
    int32_t spare = 0;

    (void)unused;
    switch (param) {
    case 9:  k = self->s3_param_def[param] >> 2; break;
    case 10: k = (self->s3_param_def[param] - 0x1f4) >> 3; break;
    case 11: k = self->s3_param_def[param] >> 4; break;
    case 12: k = self->s3_param_def[param] >> 4; break;
    default: k = spare; break;
    }

    n = self->s3_1e38;
    if (n > 0 && n != 0x7f) {
        int32_t shape = (n > 0x14) ? 0x14 : n;

        if (param == 9 && shape == 1) {
            k = from;
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, n, (uint8_t)k, (uint8_t)k);
        } else {
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, n, (uint8_t)k, (uint8_t)from);
        }
        pos = self->trk_wr[param] + self->s3_1e38;
        if (self->s3_1e38 == 1) {
            self->trk_wr[param] = pos;
            self->s3_param[param].len -= self->s3_1e38;
        }
    } else {
        pos = self->trk_wr[param];
    }

    if (cur->value == 'Y' &&
        (Phone_Attr(px3(cur->prev->value) | 0x180) & 0x40)) {
        k = (int32_t)cur->arg + 1;
        Track_BlendBack(self, self->trk_buf[param], pos, k, k,
                        (uint8_t)from);
    }

    Track_Decay(self, self->trk_buf[param], pos, 1, 1, (uint8_t)from,
                (uint8_t)from);
    pos++;

    if (hold < 1)
        hold = 1;
    k = hold;
    if (k > 0) {
        if (k > 0x14)
            k = 0x14;
        Track_RampTo(self->trk_buf[param], pos, k, (uint8_t)from,
                     (uint8_t)settle);
        pos += k;
    }

    n = self->s3_runlen - hold - 1;
    if (n > 0) {
        if (n > 0x14)
            n = 0x14;
        if (st->ctl->value == '@' && st->scan->value == 'j')
            Track_Decay(self, self->trk_buf[param], pos, n, n,
                        (uint8_t)settle, (uint8_t)settle);
        else
            Track_RampTo(self->trk_buf[param], pos, n, (uint8_t)settle,
                         (uint8_t)after);
    }

    if (param == 9) {
        /* a glide that ends up this high takes the loudness down with it */
        if ((((uint32_t)from & 0xff) << 2) > 0x28a ||
            (((uint32_t)settle & 0xff) << 2) > 0x28a ||
            (((uint32_t)after & 0xff) << 2) > 0x28a)
            self->s3_param[0].target -= 3;
    }

    if (param == 12)
        return;

    bw = (uint8_t)(self->s3_param_def[param + 4] >> 1);
    param += 4;

    n = self->s3_1e38;
    if (n > 0 && n != 0x7f) {
        int32_t shape = (n > 0x14) ? 0x14 : n;

        Track_Decay(self, self->trk_buf[param], self->trk_wr[param], shape,
                    n, bw, bw);
        pos = self->trk_wr[param] + self->s3_1e38;
        self->trk_wr[param] = pos;
        self->s3_param[param].len -= self->s3_1e38;
    } else {
        pos = self->trk_wr[param];
    }

    half = (self->s3_runlen + 1) / 2;
    k = (half > 0x14) ? 0x14 : half;
    Track_RampTo(self->trk_buf[param], pos, k, bw, (uint8_t)bw_settle);
    pos += half;

    n = self->s3_runlen - half;
    if (n > 0) {
        k = (n > 0x14) ? 0x14 : n;
        Track_Decay(self, self->trk_buf[param], pos, k, n,
                    (uint8_t)bw_settle, (uint8_t)bw_settle);
    }

    self->trk_wr[param] += self->s3_runlen;
    self->s3_param[param].len -= self->s3_runlen;
}

/* How the phoneme after a glide takes the formant over. */
/* @0x1003b09c */ extern const uint8_t g_glide_mode_sel[0x1f];

/*
 * Install one formant's end points for a glide.
 *
 * The three bytes come out of the glide tables in their own units -- the
 * first formant counts in quarters, the second from 500 in eighths, the
 * third and fourth in sixteenths -- so they are scaled here before going
 * into s3_param, along with the bandwidth that travels with them and the
 * mode saying how the phoneme after picks the formant up.
 */
/* @0x1003ab20 */
void TV_THISCALL Stage3_SetGlide(Engine *self, int32_t to, int32_t bw,
                                 int32_t from, int32_t unused3,
                                 int32_t param, int32_t plain)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    Node *prev = cur->prev;
    S3Param *p = &self->s3_param[param];
    uint8_t c_cur = cur->value;
    uint8_t c_ctl = st->ctl->value;
    uint8_t back = prev->value;
    int32_t mode = 0;
    int32_t start, target, band;
    int32_t i, v, w;

    (void)unused3;
    if (Phone_Attr(px3(back)) & 8) {
        Node *n = prev->prev;
        back = (n != NULL) ? n->value : ' ';
    }

    if ((uint8_t)plain == 0 &&
        (param == 9 ||
         (c_cur != 'm' && c_cur != 'n' && c_cur != '~' &&
          c_cur != 'B' && c_cur != 'D' && c_cur != 'G')) &&
        c_cur != 'P' && c_cur != 'T' && c_cur != 'K') {
        mode = 1;
        i = px3(c_cur) - 0x4c;
        switch (((uint32_t)i <= 0x1e) ? g_glide_mode_sel[i] : 7) {
        case 0:
        case 4:
            if (param == 9 ||
                ((Phone_Attr(px3(back)) & 0x40) && c_cur == 'R'))
                p->shape_in = (int32_t)cur->arg + 1;
            break;
        case 1:
            if (param == 10)
                mode = 0;
            /* fall through */
        case 2:
            if (param == 9)
                p->shape_in = (int32_t)cur->arg;
            break;
        case 3:
            if (param == 12) {
                uint8_t a = Phone_Attr(px3(back));

                if ((a & 0x40) || (Phone_Attr(px3(back) | 0x100) & 1)) {
                    if (!(a & 4)) {
                        p->shape_in = (int32_t)cur->arg;
                        break;
                    }
                }
                if (back == 'u' || back == '4' || back == 'c' ||
                    back == 'g')
                    p->shape_in = (int32_t)(cur->arg >> 1);
                else
                    mode = 0;
            } else if (param == 9 && (Phone_Attr(px3(back)) & 0x10)) {
                p->shape_in = (int32_t)cur->arg + 1;
            } else {
                p->shape_in = (int32_t)(cur->arg >> 1);
            }
            break;
        case 5:
            if ((Phone_Attr(px3(back) | 0x100) & 2) &&
                !(Phone_Attr(px3(back) | 0x180) & 0x40))
                mode = 0;
            break;
        case 6:
            if (param == 9 && (Phone_Attr(px3(back) | 0x100) & 2))
                p->shape_in = (int32_t)cur->arg;
            if (param == 11)
                p->shape_in = (int32_t)st->cur->arg;
            break;
        default:
            break;
        }

        if (c_cur == 'L' && (Phone_Attr(px3(self->s3_1fed)) & 0x20)) {
            v = (int32_t)(st->cur->arg / 3u);
            if ((uint32_t)v <= 2u)
                v = 2;
            p->shape_in = v;
        }
    }

    switch (param) {
    case 9:
        target = (to & 0xff) * 4;
        start = (from & 0xff) << 2;
        band = (bw & 0xff) * 2;
        break;
    case 10:
        target = ((to & 0xff) * 8) + 0x1f4;
        start = ((from & 0xff) * 8) + 0x1f4;
        band = (bw & 0xff) * 2;
        break;
    case 11:
        target = (to & 0xff) << 4;
        start = (from & 0xff) << 4;
        band = (bw & 0xff) * 2;
        break;
    case 12:
        target = (to & 0xff) << 4;
        start = (from & 0xff) << 4;
        band = mode;
        break;
    default:
        start = mode;
        target = mode;
        band = mode;
        break;
    }

    if (Phone_Attr(px3(c_cur) | 0x100) & 2) {
        /* into a vowel: the shapes come from how long the two sides are */
        if (Phone_Attr(px3(c_cur) | 0x80) & 0x10) {
            v = 1;
        } else {
            v = (int32_t)(st->cur->arg / 3u);
            if (v <= 1)
                v = 2;
        }
        if (Phone_Attr(px3(c_ctl) | 0x80) & 0x10) {
            w = 0;
        } else {
            w = (int32_t)(st->ctl->arg / 3u);
            if (w <= 1)
                w = 2;
        }
        mode = 0x10;
        p->shape_in = v;
        p->shape_out = w;
    } else if (self->trk_wr[param] != self->trk_wr[16] &&
               self->s3_1e38 == 0x7f) {
        mode = 1;
        p->start = start;
    } else if (param == 9 && c_cur == 'j') {
        p->lead = start;
        if (self->s3_param_def[9] > start + 0x64)
            p->lead = start - 0x64;
        if (p->lead < 0xfa)
            p->lead = 0xfa;
    } else {
        p->start = start;
        p->lead = start;
    }

    if (param == 9) {
        if (c_cur == 't') {
            v = p->lead - 0x64;
            p->lead = v;
            if (self->s3_param_def[9] > v)
                p->lead = v - 0x32;
            if (p->lead < 0xfa)
                p->lead = 0xfa;
        }
        if (c_cur == 'R' && (Phone_Attr(px3(back)) & 0x10))
            p->lead -= 0x14;
    }

    if (c_cur == 'R' && (Phone_Attr(px3(back) | 0x100) & 2) &&
        (param == 9 || param == 10 || param == 11))
        mode = 0;

    p->mode = mode;
    p->target = target;
    self->s3_param_def[param] = target;
    if (param != 12) {
        self->s3_param[param + 4].start = band;
        self->s3_param[param + 4].lead = band;
        self->s3_param_def[param + 4] = band;
        self->s3_param[param + 4].target = band;
        self->s3_param[param + 4].mode = 0;
    }
}

/* Where each phoneme sits in the packed glide tables, or -1 for none. */
/* @0x100bf758 */ extern const int8_t g_phone_bit[];
/* Which of the seven table groups a vowel belongs to. */
/* @0x1003bd1c */ extern const uint8_t g_gtab_sel[0x4a];
/* Stand-ins to fall back on when a pair is not in the table. */
/* @0x1003bc30 */ extern const uint8_t g_glide_alt1[0x35];
/* @0x1003bc88 */ extern const uint8_t g_glide_alt2[0x35];

/* @0x1012c440 */ extern const tv_ref g_gt0_data[];
/* @0x1012c430 */ extern const int32_t g_gt0_max[];
/* @0x1012c450 */ extern const tv_ref g_gt0_info[];
/* @0x1010b318 */ extern const tv_ref g_gt1_data[];
/* @0x1010b338 */ extern const int32_t g_gt1_max[];
/* @0x1010b348 */ extern const tv_ref g_gt1_info[];
/* @0x100aec80 */ extern const tv_ref g_gt2_data[];
/* @0x100aec70 */ extern const int32_t g_gt2_max[];
/* @0x100aeca0 */ extern const tv_ref g_gt2_info[];
/* @0x1010249c */ extern const tv_ref g_gt3_data[];
/* @0x101024b4 */ extern const int32_t g_gt3_max[];
/* @0x101024c4 */ extern const tv_ref g_gt3_info[];
/* @0x100c8914 */ extern const tv_ref g_gt4_data[];
/* @0x100c892c */ extern const int32_t g_gt4_max[];
/* @0x100c893c */ extern const tv_ref g_gt4_info[];
/* @0x100f8014 */ extern const tv_ref g_gt5_data[];
/* @0x100f8024 */ extern const int32_t g_gt5_max[];
/* @0x100f8034 */ extern const tv_ref g_gt5_info[];
/* @0x100cf098 */ extern const tv_ref g_gt6_data[];
/* @0x100cf0a8 */ extern const int32_t g_gt6_max[];

/* One of seven stand-ins, by what the phoneme is closest to. */
static uint8_t glide_alt(const uint8_t *sel, uint8_t c)
{
    int32_t i = px3(c) - 0x44;

    if ((uint32_t)i > 0x34)
        return c;
    switch (sel[i]) {
    case 0: return 't';
    case 1: return 'l';
    case 2: return 'x';
    case 3: return 'L';
    case 4: return 'j';
    case 5: return 'D';
    case 6: return 'X';
    default: return c;
    }
}

/* "j" and "l" stand in for each other, and "L" for both. */
static uint8_t glide_liquid(uint8_t c)
{
    if (c == 'L')
        return 'j';
    return (c == 'l') ? 'L' : 'l';
}

/*
 * The four formants for a vowel spanning a word boundary.
 *
 * The tables are packed by the pair of phonemes either side, so both are
 * turned into a row and a bit, and the pair is looked up.  When the pair is
 * not there, a stand-in is tried -- a closest-sounding phoneme, then the
 * other liquid -- twice over before giving up.  Returns 1 if a row was
 * found and the targets were written.
 */
/* @0x1003b4b0 */
uint8_t TV_THISCALL Stage3_GlideTab(Engine *self, int32_t which)
{
    union { int32_t d; uint8_t b[4]; } v20;
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *n, *n1, *n2;
    const uint8_t *data, *info, *p;
    const uint8_t *spare;
    uint8_t c_cur = st->cur->value;
    uint8_t c_ctl = ctl->value;
    uint8_t s1, s2, s3v, first, a1, a2;
    int32_t kind = 3;
    int32_t rank, row, bit, limit, lead, stride, off, pass, i;

    n = st->scan;
    s1 = n->value;
    n = n->next;
    s2 = n->value;
    n = n->next;
    s3v = n->value;
    if (s1 == '&' || s1 == '%') {
        n = n->next;
        s1 = s3v;
        s2 = n->value;
        s3v = n->next->value;
    } else if (s2 == '&' || s2 == '%') {
        s2 = s3v;
        n = n->next;
        s3v = n->value;
    }

    if (c_ctl == 'n')
        c_ctl = 'N';

    if (Phone_Attr(px3(s2) | 0x100) & 2) {
        /* a vowel stands in for the glide it would take */
        if (s1 == '3' || s1 == 'k' || s1 == 'r' || s1 == 'g' || s1 == '4')
            s2 = 'R';
        else if (s1 == 'b' || s1 == 'f' || s1 == 'O' || s1 == 'U')
            s2 = 'W';
        else if (s1 == '@' || s1 == 'v' || s1 == 'o' || s1 == 'w')
            s2 = ' ';
        else
            s2 = 'Y';
    }

    n1 = ctl->next;
    a1 = n1->value;
    if (Phone_Attr(px3(a1) | 0x180) & 0x40) {
        Node *e = n1->next;
        uint8_t d = e->value;

        if ((d == '&' || d == '%') && a1 != 'E' && a1 != '5') {
            d = e->next->value;
            if (d != 'R' && d != 'L' && d != 'W' && d != 'Y' && d != 'd' &&
                d != 'H' && d != 't') {
                s2 = ' ';
                goto boundary;
            }
        }
    }
    if (s2 == 'n')
        s2 = 'N';

boundary:
    n2 = n1->next;
    a2 = n2->value;
    if (a2 == '&' || a2 == '%') {
        if (n2->next->b18 == 1)
            s2 = n2->next->value;
    }

    if ((a1 == '&' || a1 == '%') && (Phone_Attr(px3(c_ctl)) & 0x40) &&
        c_ctl != 'Z' && c_ctl != 'S' && (n2->flags & 0x20u)) {
        if ((Phone_Attr(px3(a2) | 0x180) & 0x40)) {
            c_ctl = ' ';
        } else if ((Phone_Attr(px3(n2->next->value)) & 0x10) &&
                   ctl->value != 'X' &&
                   !(ctl->value == 's' && a2 == '3') &&
                   !(ctl->value == 'V' && a2 == 'a')) {
            c_ctl = ' ';
        }
    }

    row = (int32_t)g_phone_bit[px3(c_ctl)];
    if (row == -1)
        return 0;
    bit = (int32_t)g_phone_bit[px3(s2)];
    if (bit == -1)
        return 0;
    rank = Bits_Rank(bit, row, which);
    first = s2;
    v20.d = 0;
    v20.b[0] = c_ctl;

    for (pass = 0; pass < 2 && rank == -1; pass++) {
        s2 = glide_alt(g_glide_alt1, s2);
        if (s2 != first) {
            bit = (int32_t)g_phone_bit[px3(s2)];
            if (bit != -1)
                rank = Bits_Rank(bit, row, which);
        }
        if (rank == -1 && (s2 == 'j' || s2 == 'L' || s2 == 'l')) {
            s2 = glide_liquid(first);
            bit = (int32_t)g_phone_bit[px3(s2)];
            if (bit != -1)
                rank = Bits_Rank(bit, row, which);
        }
        if (rank == -1 && v20.b[0] == c_ctl) {
            s2 = first;
            bit = (int32_t)g_phone_bit[px3(first)];
            c_ctl = glide_alt(g_glide_alt2, c_ctl);
            if (v20.b[0] != c_ctl) {
                row = (int32_t)g_phone_bit[px3(c_ctl)];
                if (row != -1)
                    rank = Bits_Rank(bit, row, which);
            }
        }
        if (rank == -1 &&
            (v20.b[0] == 'j' || v20.b[0] == 'l' || v20.b[0] == 'L')) {
            c_ctl = glide_liquid(v20.b[0]);
            row = (int32_t)g_phone_bit[px3(c_ctl)];
            if (row != -1)
                rank = Bits_Rank(bit, row, which);
        }
    }

    if (s1 != '@' && s1 != '|')
        kind = 6;

    spare = (const uint8_t *)&g_phone_bit[px3(first)];
    i = px3(s1) - 0x33;
    switch (((uint32_t)i <= 0x49) ? g_gtab_sel[i] : 22) {
    case 0: case 2: case 14: case 18:
        data = TV_REF(uint8_t, g_gt4_data[which]); limit = g_gt4_max[which];
        info = TV_REF(uint8_t, g_gt4_info[which]);
        break;
    case 1: case 11: case 16:
        data = TV_REF(uint8_t, g_gt5_data[which]); limit = g_gt5_max[which];
        info = TV_REF(uint8_t, g_gt5_info[which]);
        break;
    case 3: case 21:
        data = TV_REF(uint8_t, g_gt6_data[which]); limit = g_gt6_max[which];
        info = spare;   /* the original leaves this one unset */
        break;
    case 4: case 5: case 6: case 20:
        data = TV_REF(uint8_t, g_gt1_data[which]); limit = g_gt1_max[which];
        info = TV_REF(uint8_t, g_gt1_info[which]);
        break;
    case 7: case 8: case 10: case 13:
        data = TV_REF(uint8_t, g_gt0_data[which]); limit = g_gt0_max[which];
        info = TV_REF(uint8_t, g_gt0_info[which]);
        break;
    case 9: case 12: case 15:
        data = TV_REF(uint8_t, g_gt2_data[which]); limit = g_gt2_max[which];
        info = TV_REF(uint8_t, g_gt2_info[which]);
        break;
    case 17: case 19:
        data = TV_REF(uint8_t, g_gt3_data[which]); limit = g_gt3_max[which];
        info = TV_REF(uint8_t, g_gt3_info[which]);
        break;
    default:
        /* all three left unset in the original */
        data = spare; info = spare; limit = (int32_t)(uintptr_t)spare;
        break;
    }

    if (s1 == '@' || s1 == '|') {
        lead = 6;
    } else {
        /* the original also works out how wide each of the four fields is,
         * but never uses the answers */
        lead = (int32_t)info[0];
        v20.d = (int32_t)info[9];
    }

    off = 0;
    if (rank != -1) {
        stride = kind + lead + 9;
        off = stride * rank;
        i = (int32_t)data[lead + kind + off + 8];
        i = Stage3_FindRow(self, i, which, c_cur, s3v, v20.d);
        if (i != -1) {
            rank = i;
            off = stride * i;
        }
    }

    if (rank == -1 || rank >= limit)
        return 0;

    p = data + lead + off;
    self->s3_param[9].target = (int32_t)p[0] << 2;
    self->s3_param[10].target = ((int32_t)p[1] << 3) + 0x1f4;
    self->s3_param[11].target = (int32_t)p[2] << 4;
    self->s3_param[12].target = (int32_t)p[3] << 4;
    return 1;
}

/* Which of six routines each stop calls for. */
/* @0x1004b5b0 */ extern const uint8_t g_op9_sel[0x13];

/*
 * The stop bursts.
 *
 * Releasing a stop lets the air behind the closure out as a short noise.
 * The first half works out how loud it is in each of the three bands --
 * s3_1e5c -- from where the closure was and what follows it; the second half
 * writes those levels into the tracks over the frames the burst lasts.
 */
/* @0x1004a200 */
void TV_THISCALL Stage3_Op9(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur, *scan;
    uint8_t c, d, a;
    int32_t i, v, n, step;

    i = px3(ctl->value) - 0x42;
    switch (((uint32_t)i <= 0x12) ? g_op9_sel[i] : 6) {
    case 0:                                     /* "B" */
        scan = st->scan;
        c = scan->value;
        if (c == 'p') {
            self->s3_1e5c[0] = 0x7f;
            self->s3_1e5c[2] = 0;
            self->s3_1e5c[1] = 0x39;
            if (st->cur->value == 'S')
                self->s3_1e5c[1] = 0x3c;
        } else if ((Phone_Attr(px3(c) | 0x100) & 2) &&
                   st->cur->value == 'S') {
            self->s3_1e5c[0] = 0x7f;
            self->s3_1e5c[1] = 0x3c;
        } else if (c == 'Z' && st->cur->value != 'S') {
            self->s3_1e5c[1] += 8;
        } else if (Phone_IsVowel(c)) {
            self->s3_1e5c[1] = 0x3e;
        } else if (st->scan->value == 'i') {
            self->s3_1e5c[1] = 0x3a;
        } else if (st->scan->value == 'X') {
            self->s3_1e5c[1] = 0x44;
        }
        if (Phone_Attr(px3(st->scan->value) | 0x100) & 2) {
            c = st->cur->value;
            if (c == 'P' || c == 'T' || c == 'K')
                self->s3_param[0].target = 0;
        }
        if (st->scan->value == 'k') {
            self->s3_1e5c[1] = 0x44;
            self->s3_1e5c[2] = 0x37;
        }
        break;

    case 1:                                     /* "D" */
        d = st->scan->value;
        if (d == 'R' || d == 'L' || d == 'W' || d == 'Y') {
            if (st->cur->value == 'S' && d == 'R' &&
                (ctl->flags & 0x20u)) {
                self->s3_1e3c = 2;
                self->s3_param[5].target = 0x41;
                self->s3_1e5c[1] = 0x3c;
                self->s3_param[7].target = 0x3c;
                self->s3_param[6].target = 0x3c;
            } else {
                self->s3_1e3c = 1;
                self->s3_1e5c[0] = 0x7f;
                self->s3_1e5c[1] = 0x3c;
            }
            break;
        }
        c = ctl->next->value;
        if ((c == '&' || c == '%') &&
            ((Phone_Attr(px3(d) | 0x100) & 2) ||
             (d == 'x' && st->cur->value == 'N'))) {
            self->s3_1e5c[1] = 0x2a;
        } else if (d == '|') {
            self->s3_1e5c[0] = 0x32;
            self->s3_1e5c[1] = 0x3c;
            self->s3_1e5c[2] = 0x30;
        } else if (d == 'X') {
            self->s3_1e5c[1] = 0x44;
        } else if (d == 'Z') {
            if (Phone_Attr(px3(st->cur->value) | 0x100) & 2) {
                self->s3_1e5c[1] = 0x46;
                self->s3_1e5c[2] = 0x3c;
            }
        } else if (d == 'M') {
            if (Phone_Attr(px3(c)) & 8) {
                self->s3_1e5c[1] = 0x41;
                self->s3_param[5].target = 0x41;
                self->s3_1e3c = 1;
            }
        } else if (d == 'w') {
            if (Phone_Attr(px3(st->cur->value) | 0x100) & 2)
                self->s3_1e5c[1] = 0x3c;
        }
        break;

    case 2:                                     /* "G" */
        self->s3_param[1].start = 0;
        self->s3_param[1].target = 0;
        c = st->scan->value;
        if (c == 'Z') {
            self->s3_1e5c[0] = 0x32;
        } else if (c == 'R' || c == 'L') {
            self->s3_1e5c[1] = 0x42;
        } else if (c == 'X') {
            self->s3_1e5c[1] = 0x41;
            self->s3_1e5c[2] = 0x39;
        }
        if ((Phone_Attr(px3(st->cur->value) | 0x100) & 2) &&
            (Phone_Attr(px3(st->scan->value)) & 0x10)) {
            self->s3_1e5c[1] = 0x32;
            self->s3_1e5c[2] = 0x28;
        }
        if ((Phone_Attr(px3(st->cur->value) | 0x100) & 2) &&
            ctl->next->value == 'p')
            self->s3_1e5c[0] = 0x2e;
        break;

    case 3:                                     /* "K" */
        self->s3_param[1].start = 0;
        self->s3_param[1].target = 0;
        scan = st->scan;
        c = scan->value;
        if (c == 'p') {
            self->s3_1e5c[1] = 0x3a;
            self->s3_1e5c[2] = 0x30;
            self->s3_param[8].target = 0x37;
        } else if (c == 'A') {
            self->s3_1e5c[1] = 0x3c;
        } else if (c == 'a') {
            self->s3_1e5c[1] = 0x40;
        } else if (c == 'k') {
            self->s3_1e5c[1] = 0x40;
            self->s3_param[5].target = 0x37;
            self->s3_param[4].target = 0x44;
            self->s3_param[6].target = 0x44;
        } else if (c == 'L') {
            self->s3_1e5c[1] = 0x41;
            self->s3_param[6].target = 0x43;
            self->s3_param[7].target = 0x41;
            if ((Phone_Attr(px3(st->cur->value) | 0x100) & 2) &&
                (Phone_Attr(px3(ctl->next->value)) & 8))
                self->s3_1e5c[1] = 0x37;
        } else if (c == 'T') {
            self->s3_1e5c[1] = 0x41;
            self->s3_1e5c[2] = 0x3e;
            self->s3_1e3c = 1;
        } else if (c == 'e') {
            self->s3_param[5].target = 0x37;
            self->s3_param[6].target = 0x37;
            self->s3_1e5c[1] = 0x44;
            self->s3_param[4].target = 0x41;
            if (Phone_Attr(px3(st->cur->value) | 0x100) & 2)
                self->s3_1e5c[1] = 0x3e;
        } else if (c == '|') {
            self->s3_1e5c[1] = 0x41;
            self->s3_param[4].target = 0x46;
            self->s3_param[5].target = 0x23;
            self->s3_param[6].target = 0x37;
            self->s3_param[7].target = 0x32;
        } else if (c == 'b' || c == 'u' || c == 'O' || c == 'U') {
            self->s3_1e5c[1] = 0x41;
        } else if ((Phone_Attr(px3(c) | 0x100) & 2) &&
                   (scan->flags & 0x20u)) {
            self->s3_1e5c[1]++;
            self->s3_1e5c[2]++;
        } else if (c == 'R') {
            self->s3_1e5c[1] = 0x3f;
            self->s3_param[3].target = 0x3f;
        } else if (c == 'Y') {
            self->s3_param[4].target = 0x23;
            self->s3_param[5].target = 0x2d;
            self->s3_param[6].target = 0x46;
            self->s3_param[7].target = 0x50;
        } else if (c == 'M' || c == 'N') {
            self->s3_1e5c[1] = 0x7f;
        }

        if ((Phone_Attr(px3(st->cur->value) | 0x100) & 2) &&
            scan->value == 'W') {
            self->s3_param[10].target = 0x640;
            self->s3_1e5c[1] = 0x41;
            self->s3_param[3].target = 0x3c;
            self->s3_param[7].target = 0x3c;
        }
        if (scan->value == 'w') {
            self->s3_param[3].target = 0x3c;
            self->s3_param[6].target = 0x41;
            self->s3_1e5c[1] = 0x3c;
        }
        if (scan->value == 's' &&
            (Phone_Attr(px3(st->cur->value) | 0x100) & 2)) {
            self->s3_1e5c[1] = 0x46;
            self->s3_1e5c[2] = 0x37;
            d = scan->next->value;
            if (d == 'E' || d == 'a' || d == 'i' || d == 'e' || d == 'A' ||
                d == '4' || d == 'k') {
                self->s3_param[3].target = 0x2d;
                self->s3_param[5].target = 0x41;
                self->s3_param[4].target = 0x4b;
                self->s3_param[6].target = 0x4b;
            }
        }
        if (scan->value == 'i') {
            self->s3_1e5c[1] += 7;
            for (i = 4; i < 7; i++)
                self->s3_param[i].target += 7;
        }
        break;

    case 4:                                     /* "P" */
        scan = st->scan;
        c = scan->value;
        if (c == 'y') {
            self->s3_1e5c[1] = 0x3a;
            self->s3_1e5c[2] += 3;
        } else if ((Phone_Attr(px3(c) | 0x100) & 2) &&
                   (scan->flags & 0x20u)) {
            self->s3_1e5c[1] += 3;
            self->s3_1e5c[2] += 3;
        } else if (c == 'S') {
            self->s3_1e5c[1] = 0x3c;
        } else if (c == 'Y') {
            self->s3_param[3].target = 0x50;
        }
        a = Phone_Attr(px3(scan->value));
        if (!(a & 4) && (a & 0x60)) {
            self->s3_1e5c[1] = 0x46;
            self->s3_1e5c[2] = 0x3e;
            if (Phone_Attr(px3(scan->value) | 0x100) & 4)
                self->s3_param[10].target = 0x38c;
        }
        if (ctl->prev->value == 'E' && scan->value == 'R') {
            self->s3_1e5c[1] = 0x41;
            self->s3_param[8].target = 0x3c;
            self->s3_param[3].target = 0;
            self->s3_param[4].target = 0;
            self->s3_param[5].target = 0;
            self->s3_param[6].target = 0;
            self->s3_param[7].target = 0;
        }
        if (ctl->prev->value == 'E' && scan->value == 's') {
            self->s3_1e5c[1] = 0x41;
            self->s3_param[3].target = 0x41;
        }
        if (scan->value == 'a') {
            self->s3_param[4].target = 0x41;
            self->s3_param[5].target = 0x2d;
            self->s3_1e5c[1] = 0x43;
            self->s3_param[3].target = 0x37;
            self->s3_1e5c[2] = 0x37;
        }
        break;

    case 5:                                     /* "T" */
        scan = st->scan;
        c = scan->value;
        if ((Phone_Attr(px3(c) | 0x100) & 2) &&
            ((scan->flags & 0x20u) || c == 'p')) {
            if (c == 'b') {
                self->s3_1e5c[1] = 0x41;
            } else if (!(st->cur->value == 'N' && self->s3_1e5c[1] == 0)) {
                self->s3_1e5c[1] = 0x3a;
            }
            self->s3_param[8].target = 0;
        }
        if (Phone_Attr(px3(scan->value) | 0x200) & 2) {
            self->s3_1e5c[1] = 0x41;
            self->s3_1e5c[2] = 0x34;
        } else if (Phone_Attr(px3(scan->value) | 0x100) & 2) {
            self->s3_1e5c[1] = 0x3f;
            self->s3_1e5c[2] = 0x37;
        }
        cur = st->cur;
        if (cur->value == 'F' && scan->value == 'p') {
            self->s3_1e5c[1] = 0x41;
            self->s3_1e5c[2] = 0x37;
        }
        if (scan->value == 'n') {
            self->s3_1e5c[1] = 0x3c;
            self->s3_1e5c[2] = 0x30;
        }
        if ((Phone_Attr(px3(cur->value) | 0x100) & 2) &&
            scan->value == 'K') {
            self->s3_1e5c[1] = 0x44;
            self->s3_1e5c[2] = 0x3a;
            self->s3_1e3c = 1;
        }
        if (cur->value == 'S' &&
            (Phone_Attr(px3(scan->value) | 0x100) & 2)) {
            self->s3_1e5c[1] = 0x41;
            self->s3_1e5c[2] = 0x37;
        }
        if (scan->value == 'c') {
            c = ctl->prev->value;
            if (c == '&' || c == '%' || c == ' ') {
                self->s3_param[4].target = 0x41;
                self->s3_1e5c[1] = 0x3c;
            }
        }
        if (scan->value == 'r') {
            self->s3_param[3].target = 0x28;
            self->s3_param[4].target = 0x46;
            self->s3_param[7].target = 0x46;
        }
        if (cur->value == 'N' && !(cur->prev->flags & 0x20u) &&
            (Phone_Attr(px3(cur->prev->value) | 0x100) & 2)) {
            c = ctl->next->value;
            if ((c == '%' || c == '&') &&
                (Phone_Attr(px3(scan->value) | 0x100) & 2) &&
                (scan->next->flags & 0x20u))
                self->s3_1e3c = 0x7f;
        }
        break;

    default:
        break;
    }

    ctl = st->ctl;
    c = ctl->value;
    if (c == 'B' || c == 'D' || c == 'G') {
        scan = st->scan;
        if (scan->value == 'Z' && scan->next->value == ' ') {
            self->s3_param[0].target = 0x37;
            self->s3_1e5c[0] = 0;
        } else {
            d = st->cur->value;
            if (d != ' ' && (Phone_Attr(px3(d)) & 4))
                self->s3_param[0].target = 0x2a;
        }
        if (st->cur->value == 'Z') {
            d = st->cur->prev->value;
            if (d == 'B' || d == 'D' || d == 'G')
                self->s3_param[0].mode &= 6;
        }
    }
    if (ctl->value == 'B' &&
        (Phone_Attr(px3(st->cur->value) | 0x100) & 2) &&
        ctl->next->value == 'p') {
        self->s3_param[0].target = 0x29;
        self->s3_param[0].mode = 4;
        self->s3_1e5c[0] = 0x2b;
    }
    if (!(st->p_38 & 1) && self->s3_1e5c[1] != 0x7f &&
        self->s3_1e5c[1] < 0)
        self->s3_1e5c[1] = 0;

    step = 0;
    if (!(ctl->flags & 0x20u)) {
        d = ctl->value;
        if (!(d == 'C' && st->scan->value == 's' &&
              st->scan->next->value == 'R') &&
            (d == 'C' || d == 'J')) {
            step = 3;
            if (Phone_Attr(px3(st->cur->value)) & 0x10)
                step = 6;
        }
    }

    if (ctl->value == 'C' && st->scan->value == 's') {
        c = ctl->prev->value;
        if (c == ' ' || c == '&' || c == '%') {
            d = st->scan->next->value;
            if (d == '3' || d == 'v' || d == 'A' || d == 'I' || d == 'k' ||
                d == 'r' || d == 'O' || d == 'w' || d == 'c' || d == 'g' ||
                d == '4') {
                self->s3_param[4].target = 0x3c;
                self->s3_param[3].target = 0;
                self->s3_param[5].target = 0;
                self->s3_param[6].target = 0;
                self->s3_param[7].target = 0;
            } else if (d == 'i' || d == 'E' || d == '5' || d == 'o' ||
                       d == 'e' || d == 'f' || d == 'y') {
                self->s3_param[5].target = 0x37;
                self->s3_param[7].target = 0x2d;
                self->s3_param[3].target = 0;
                self->s3_param[4].target = 0;
                self->s3_param[6].target = 0;
            } else if (d == 'b' || d == 'u') {
                self->s3_param[4].target = 0x2d;
                self->s3_param[3].target = 0;
                self->s3_param[5].target = 0;
                self->s3_param[6].target = 0;
                self->s3_param[7].target = 0;
            }
        }
    }

    n = self->s3_param[1].len;
    if (n <= self->s3_1e3c && self->s3_1e3c != 0x7f)
        self->s3_1e3c = n >> 1;
    if (self->s3_1e3c <= 0)
        self->s3_1e3c = 1;
    Track_Nudge(self, self->trk_buf[1], 1, self->trk_wr[1], 3,
                self->trk_wr[1] - self->trk_rd[1], -0x14);

    if (n > self->s3_1e3c) {
        n -= self->s3_1e3c;
        for (i = 0; i < 3; i++) {
            int32_t done = 0;

            if (self->s3_1e5c[i] == 0x7f)
                continue;
            v = self->s3_param[i].target - st->volume_atten;
            self->s3_param[i].target = v;
            if (v < 0)
                self->s3_param[i].target = 0;

            cur = st->cur;
            scan = st->scan;
            if (i == 0 && cur->value == ' ') {
                /* the original tests here for a "D" before a vowel, but
                 * its own guard on a space can never hold at the same time */
                if (ctl->value == 'B' && n > 3) {
                    Track_Fill(self->trk_buf[i], self->trk_wr[i], n - 3, 0);
                    Track_Fill(self->trk_buf[i], self->trk_wr[i] + n - 3, 3,
                               (uint8_t)self->s3_param[i].target);
                } else {
                    Track_Fill(self->trk_buf[i], self->trk_wr[i], n,
                               (uint8_t)self->s3_param[i].target);
                    Track_BlendFwd(self, self->trk_buf[0], self->trk_wr[0],
                                   n >> 1, n,
                                   (uint8_t)(self->s3_param[0].target >> 1));
                }
                done = 1;
            }
            if (!done && i == 0) {
                c = ctl->value;
                if ((c == 'B' || c == 'D' || c == 'G') &&
                    scan->value == 'Z' && scan->next->value == ' ') {
                    Track_Fill(self->trk_buf[i], self->trk_wr[i], n,
                               (uint8_t)self->s3_param[i].target);
                    Track_Nudge(self, self->trk_buf[0], 0,
                                self->trk_wr[0] + n, n + 2, n + 2, -0xc);
                    done = 1;
                }
            }
            if (!done && i == 0 && ctl->value == 'T' &&
                (Phone_Attr(px3(cur->value) | 0x100) & 2) &&
                (Phone_Attr(px3(scan->value) | 0x100) & 2)) {
                Track_BlendBack(self, self->trk_buf[i], self->trk_wr[i], 6,
                                6, 0x28);
                Track_Fill(self->trk_buf[i], self->trk_wr[i], n,
                           (uint8_t)self->s3_param[i].target);
                done = 1;
            }
            if (!done && i == 0) {
                c = ctl->value;
                if ((c == 'B' || c == 'D') &&
                    (Phone_Attr(px3(cur->value)) & 4) &&
                    !(cur->value == 'Z' &&
                      (cur->prev->value == 'B' || cur->prev->value == 'D' ||
                       cur->prev->value == 'G'))) {
                    v = self->s3_param_def[0] / 4 -
                        self->s3_param[0].target / 4;
                    Track_BlendBack(self, self->trk_buf[i], self->trk_wr[i],
                                    (int32_t)(cur->arg >> 1),
                                    self->trk_wr[0] - self->trk_rd[0],
                                    (uint8_t)(self->s3_param[0].target + v));
                    Track_Fill(self->trk_buf[i], self->trk_wr[i], n,
                               (uint8_t)self->s3_param[i].target);
                    done = 1;
                }
            }
            if (!done) {
                if (ctl->value == 'q' &&
                    (!(Phone_Attr(px3(scan->value)) & 4) ||
                     scan->value == 'x') &&
                    (Phone_Attr(px3(cur->value) | 0x100) & 2)) {
                    v = (int32_t)(cur->arg >> 2);
                    Track_BlendBack(self, self->trk_buf[i], self->trk_wr[i],
                                    v, v, 0x26);
                }
                Track_Fill(self->trk_buf[i], self->trk_wr[i], n,
                           (uint8_t)self->s3_param[i].target);
            }

            self->trk_wr[i] += n;
            self->s3_param[i].len -= n;
            self->s3_param_def[i] = self->s3_param[i].target;
            self->s3_1e5c[i] -= step;
            if (self->s3_1e5c[i] < 0)
                self->s3_1e5c[i] = 0;

            c = ctl->value;
            if (i == 0 &&
                (((c == 'D' || c == 'G' || c == 'J') &&
                  cur->value == ' ') ||
                 (c == 'G' &&
                  (Phone_Attr(px3(cur->value) | 0x80) & 0x20) &&
                  !(Phone_Attr(px3(cur->value)) & 4))))
                self->s3_param[i].target = 0;
            else
                self->s3_param[i].target = self->s3_1e5c[i];

            if (i == 0 && scan->value == 'p') {
                self->s3_param[i].mode = 5;
            } else if (i == 0 && (scan->flags & 0x20u) &&
                       ctl->value == 'D' && cur->value != ' ' &&
                       g_class_kind[TV_REF(uint8_t, g_phone_class)[px3(scan->value)]] == 1) {
                self->s3_param[i].mode = 5;
            } else {
                self->s3_param[i].mode = 4;
            }
        }
    }

    c = ctl->value;
    if ((c == 'P' && (ctl->next->value == 'R' ||
                      ctl->next->value == 'L')) ||
        (c == 'T' && (st->scan->flags & 0x20u) &&
         (Phone_Attr(px3(st->scan->value) | 0x100) & 2))) {
        Track_Fill(self->trk_buf[8], self->trk_wr[8], n, 0);
        self->s3_param[8].target = (ctl->value == 'P') ? 0x3c : 0x3e;
        if (ctl->value == 'T') {
            d = ctl->prev->value;
            if ((d == ' ' || d == '%' || d == '&') &&
                (Phone_Attr(px3(st->scan->value) | 0x100) & 2))
                self->s3_param[8].target = 0x37;
        }
        self->trk_wr[8] += n;
        self->s3_param[8].len -= n;
        self->s3_param[8].mode = 4;
        self->s3_param_def[8] = 0;
    }

    if (ctl->value == 'B' &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2))
        self->s3_param[8].target = 0x3c;
    if (ctl->value == 'T' && ctl->prev->value == 'S' &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2)) {
        d = ctl->next->value;
        if (d == '%' || d == '&')
            self->s3_param[8].target = 0;
    }
    if (st->scan->value == 'O' && st->cur->value == ' ' &&
        ctl->value == 'T')
        self->s3_param[8].target = 0;
    self->s3_1e50 = n;
}

/* Which rule the phoneme just finished calls for. */
/* @0x1004f68c */ extern const uint8_t g_op14_sel[0x3b];
/* And which one the phoneme coming up calls for. */
/* @0x1004f6e0 */ extern const uint8_t g_op14_cur_sel[0x25];

/*
 * Where each parameter starts this phoneme.
 *
 * Every parameter begins halfway between where it settled last time and
 * where it is headed, kept within a few units of both.  What follows is the
 * exceptions: the amplitudes, which have to fall away before a closure and
 * come back after it, and the handful of pairs the halfway rule gets wrong.
 */
/* @0x1004e360 */
void TV_THISCALL Stage3_Op14(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *n;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    int32_t c_scan = px3(st->scan->value);
    int32_t back;
    uint8_t a, b, cv;
    int32_t i, v, lim;
    int32_t w = 0, mode0 = 0;

    n = cur->prev;
    back = px3(n->value);
    if (Phone_Attr(back) & 8) {
        n = n->prev;
        back = (n != NULL) ? px3(n->value) : 0x20;
    }

    for (i = 0; i < 0x16; i++) {
        if (i >= 9 && i <= 0x11)
            continue;
        lim = 9;
        v = (self->s3_param_def[i] + self->s3_param[i].target) / 2;
        self->s3_param[i].start = v;
        if (c_ctl == ' ')
            lim = 0xf;
        if (i == 2)
            lim = 0;
        if (!(Phone_Attr(c_cur) & 4))
            lim = 0xa;
        if (i > 2)
            lim = 0;
        if (i == 0 && (Phone_Attr(c_ctl) & 0x10) && c_cur == ' ')
            lim = 0x14;
        if (i <= 9) {
            w = self->s3_param[i].target - lim;
            if (w > v)
                self->s3_param[i].start = w;
            w = self->s3_param_def[i] - lim;
            if (self->s3_param[i].start < w)
                self->s3_param[i].start = w;
        }
        self->s3_param[i].lead = self->s3_param[i].start;
    }

    if ((Phone_Attr(c_cur) & 4) && c_cur != 'R') {
        v = self->s3_param[0].target;
        if (v != 0) {
            self->s3_param[0].lead = v;
        } else {
            self->s3_param[0].start = 0x28;
            self->s3_param[0].lead = 0x28;
        }
        a = Phone_Attr(c_ctl);
        if (!((a & 4) && (a & 2)) && !(c_ctl == ' ' && c_cur == 'z')) {
            v = self->s3_param[0].lead - 6;
            self->s3_param[0].lead = v;
            if (!(c_ctl == 'V' &&
                  (Phone_Attr(px3(st->scan->value) | 0x100) & 2)))
                self->s3_param[0].start = v;
        }
        if (Phone_IsVowel(st->cur->value)) {
            v = (int32_t)(st->cur->arg / 3u);
            self->s3_param[0].shape_in = v;
            if (v < 2)
                self->s3_param[0].shape_in = 2;
        } else {
            self->s3_param[0].shape_in = (int32_t)(st->cur->arg >> 2);
        }
        if ((Phone_Attr(c_ctl) & 2) && (Phone_Attr(c_cur) & 0x20) &&
            (Phone_Attr(c_ctl | 0x100) & 2)) {
            self->s3_param[0].start = 0x3c;
            if (((c_cur == 'G' || c_cur == 'D') && c_ctl == 'E') ||
                c_ctl == 'u' || c_ctl == 'b' ||
                (c_cur == 'G' && !(Phone_Attr(c_ctl | 0x200) & 1) &&
                 !(Phone_Attr(c_ctl | 0x100) & 0x20) &&
                 c_ctl != 'p' && c_ctl != 'o'))
                self->s3_param[0].start = 0x38;
            if (c_cur == 'B' && (Phone_Attr(c_ctl | 0x200) & 1) &&
                !(Phone_Attr(c_ctl | 0x100) & 0x20)) {
                self->s3_param[14].lead = 0x4b;
                self->s3_param[13].lead = 0x4b;
                self->s3_param[14].start = 0x4b;
                self->s3_param[13].start = 0x4b;
            }
        }
    }

    a = Phone_Attr(c_ctl);
    if (a & 4) {
        if (!(a & 0x20) && !(Phone_Attr(c_ctl | 0x180) & 2) &&
            !(Phone_Attr(c_scan | 0x100) & 1) && c_scan != ' ') {
            int32_t seven = 0;

            if (!((Phone_Attr(c_cur) & 0x20) &&
                  !(Phone_Attr(c_cur | 0x180) & 2) &&
                  !(Phone_Attr(c_ctl | 0x100) & 1) && c_ctl != ' ') &&
                c_cur != ' ') {
                self->s3_param[0].mode = 7;
                seven = 1;
            }
            if (!seven && !(c_ctl == 'L' && c_cur == 'P'))
                self->s3_param[0].mode = 6;
            if (c_scan == ' ' || (ctl->flags & 0x20u))
                self->s3_param[0].shape_out = (int32_t)(ctl->arg >> 2);
            else
                self->s3_param[0].shape_out = (int32_t)(ctl->arg >> 1);
            self->s3_param[0].shape_in = (int32_t)(cur->arg >> 2);
        }

        if (c_cur == 'B') {
            v = self->s3_param_def[0];
            if (v != 0) {
                self->s3_param[0].mode |= 2;
                self->s3_param[0].start = v;
                self->s3_param[0].shape_out = (int32_t)(ctl->arg >> 1);
            } else {
                self->s3_param[0].mode = 4;
            }
        }
        if (Phone_Attr(px3(ctl->value)) & 0x40) {
            b = Phone_Attr(px3(st->scan->value));
            if ((b & 0x40) && !(b & 4)) {
                self->s3_param[0].target = 0;
                self->s3_param[0].mode = 6;
                self->s3_param[0].shape_out = (int32_t)(ctl->arg / 3u);
            }
        }
        cv = st->cur->value;
        if (((cv == 'D' || cv == 'G' || cv == 'J') &&
             self->s3_1fed == ' ') ||
            (cv == 'G' &&
             (Phone_Attr(px3(self->s3_1fed) | 0x80) & 0x20) &&
             !(Phone_Attr(px3(self->s3_1fed)) & 4)))
            self->s3_param[0].mode = 6;
    }

    i = c_ctl - 0x44;
    switch (((uint32_t)i <= 0x3a) ? g_op14_sel[i] : 21) {
    case 0:
        if (st->scan->value == 'o')
            self->s3_param[0].start = 0;
        if (!(ctl->flags & 0x20u) && (Phone_Attr(c_cur) & 2) &&
            (Phone_Attr(c_scan) & 2)) {
            self->s3_param[8].lead = 0;
            self->s3_param[8].start = 0;
        }
        break;
    case 1:
        if (c_cur == 'R')
            self->s3_param[0].start = 0x39;
        break;
    case 2:
        if (c_cur == ' ') {
            self->s3_param[8].lead = 0xf;
            self->s3_param[8].start = 0xf;
        }
        break;
    case 3:
        v = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_scan]];
        if (v == 0 || v == 1) {
            self->s3_param[2].start = 0xa;
            self->s3_param[2].shape_out = 5;
        }
        break;
    case 4: case 6: case 9:
        if (!Phone_IsVowel((uint8_t)c_cur)) {
            self->s3_param[0].start = 0;
            self->s3_param[0].mode = 4;
        }
        break;
    case 5:
        if (c_cur == 'F') {
            self->s3_param[0].target = 0x39;
            self->trk_wr[0] -= 2;
            self->s3_param[0].len += 2;
            self->s3_param[0].mode = 5;
            self->s3_param[0].lead = 0x39;
        }
        break;
    case 7:
        if (c_cur == 'z') {
            v = self->s3_param_def[0];
            self->s3_param[0].start = v;
            self->s3_param[0].lead = v;
        }
        b = Phone_Attr(c_cur);
        if ((b & 0x40) && !(b & 4)) {
            self->s3_param[0].shape_in += 4;
            self->s3_param[0].lead += 0xa;
        }
        if (c_cur == 'X') {
            self->s3_param[0].lead = 0x39;
            self->s3_param[0].start = 0x39;
            self->s3_param[1].start = 0;
        }
        break;
    case 8:
        if (c_cur == ' ') {
            self->s3_param[1].mode = 6;
            self->s3_param[1].start = 0x10;
        }
        self->s3_param[1].shape_in = (int32_t)(st->cur->arg >> 2);
        break;
    case 10:
        if (st->scan->value == 'E' && c_cur == ' ') {
            self->s3_param[11].start = 0x866;
            self->s3_param[15].start = 0xfa;
        }
        break;
    case 11:
        if (st->scan->value == 'R') {
            self->s3_param[1].lead = 0x28;
            self->s3_param[1].start = 0x28;
        }
        if (Phone_Attr(c_cur | 0x100) & 2) {
            self->s3_param[9].mode = 6;
            self->s3_param[9].start = self->s3_param_def[9];
        }
        if (st->scan->value == 'R') {
            self->s3_param[6].target = 0x48;
            self->s3_param[7].target = 0x50;
            self->s3_param[3].target = 0;
            self->s3_param[4].target = 0;
            self->s3_param[5].target = 0;
        }
        break;
    case 12:
        if (Phone_Attr(c_cur) & 4) {
            self->s3_param[0].mode = 7;
            self->s3_param[0].shape_out = 6;
            if (Phone_Attr(c_cur | 0x100) & 2) {
                self->s3_param[0].lead = 0x34;
                self->s3_param[0].start = 0x34;
                if (c_scan == ' ')
                    break;
                self->s3_param[0].shape_in = (int32_t)(st->cur->arg >> 2);
            }
        }
        if (c_scan == ' ')
            break;
        self->s3_param[0].lead = 0x2a;
        self->s3_param[1].lead = 0xa;
        self->s3_param[1].start = 0xa;
        self->s3_param[0].start = 0x2a;
        if (!(Phone_Attr(c_cur | 0x100) & 2) &&
            !(Phone_Attr(c_scan | 0x100) & 2)) {
            self->s3_param[17].shape_in = 4;
            self->s3_param[17].shape_out = 4;
        }
        break;
    case 13:
        if (c_cur == 'B')
            self->s3_param[10].start += 0x64;
        break;
    case 14:
        if (c_cur == 'D') {
            self->s3_param[18].target = 0xa;
            self->s3_param[19].target = 0xf;
        } else if (c_cur == 'P' || c_cur == 'T' || c_cur == 'K') {
            self->s3_param[1].start = self->s3_param_def[1];
            self->s3_param[1].shape_out = self->s3_param[1].len;
            self->s3_param[1].target = 0x2a;
        } else if (c_cur == 'G' || c_cur == 'B') {
            self->s3_param[0].target = 0x32;
            self->s3_param[0].start = 0x32;
            self->s3_param[0].mode = 6;
        }
        break;
    case 15:
        self->s3_param[0].mode = 4;
        self->s3_param[0].target = 0;
        break;
    case 16:
        if ((c_cur == 'C' && (Phone_Attr(c_scan | 0x100) & 2)) ||
            c_scan == '@') {
            for (i = 0; i < 4; i++) {
                self->s3_param[4 + i].lead = self->s3_param_def[4 + i];
                self->s3_param[4 + i].start = self->s3_param_def[4 + i];
            }
            if (c_scan == ' ') {
                self->s3_param[1].start = 0x23;
                self->s3_param[1].lead = 0x19;
                self->s3_param[1].shape_out = 6;
                self->s3_param[1].shape_in = 3;
                self->s3_param[1].target = 0x42;
                self->s3_param[1].mode = 7;
            }
        }
        break;
    case 17:
        self->s3_param[0].mode = 4;
        break;
    case 18:
        cv = st->cur->value;
        v = 0;
        if (cv == 'q') {
            v = 1;
        } else {
            b = Phone_Attr(px3(cv));
            if (!(b & 4)) {
                if (b & 0x40)
                    v = 1;
                else if (Phone_Attr(px3(cv) | 0x100) & 1)
                    v = 1;
            }
        }
        if (v) {
            self->s3_param[0].target = 0;
            self->s3_param[0].mode = 4;
        }
        break;
    case 19:
        if (c_cur == 'P') {
            for (i = 0; i < 5; i++)
                self->s3_param[3 + i].target = self->s3_param_def[3 + i];
        }
        break;
    case 20:
        if (Phone_Attr(c_cur | 0x100) & 0x20)
            self->s3_param[10].lead = 0x834;
        break;
    default:
        break;
    }

    if ((Phone_Attr(c_ctl | 0x100) & 2) && (ctl->flags & 0x40u)) {
        self->s3_param[0].start = 0x2a;
        self->s3_param[0].shape_out = 4;
        self->s3_param[0].mode = 6;
    }
    if (c_cur == 'Z') {
        self->s3_param[18].start = self->s3_param_def[18];
        self->s3_param[18].shape_out = 5;
        if (ctl->arg < 5u)
            self->s3_param[18].shape_out = (int32_t)ctl->arg;
    }

    if (c_scan == ' ') {
        a = Phone_Attr(c_ctl);
        if ((a & 4) && (a & 0x40)) {
            int32_t ok = 1;
            cv = st->scan->next->value;

            if (((Phone_Attr(px3(cv) | 0x180) & 1) || cv == 'M' ||
                 cv == 'N') && !(Phone_Attr(c_scan) & 8))
                ok = 0;
            if (ok) {
                cv = st->cur->value;
                if (c_ctl == 'Z' &&
                    (cv == 'B' || cv == 'D' || cv == 'G')) {
                    self->s3_param[0].target = 0;
                    self->s3_param[0].mode = 4;
                } else if (!(Phone_Attr(px3(cv)) & 0x50) && cv != 'j' &&
                           cv != 'l' && ctl->arg > 6u) {
                    v = self->s3_param[0].len;
                    self->s3_param[0].len = 4;
                    self->s3_param[0].start = 0x3c;
                    self->s3_param[0].target = 0x32;
                    self->s3_param[0].mode = 6;
                    Stage3_Write(self, 0);
                    self->trk_wr[0] += 4;
                    self->s3_param_def[0] = 0x32;
                    self->s3_param[0].mode = 4;
                    self->s3_param[0].len = v - 4;
                } else {
                    self->s3_param[0].lead = 0x32;
                    self->s3_param[0].shape_in = 4;
                    self->s3_param[0].mode = 5;
                }
                self->s3_param[0].target = 0;
            }
        }
    }

    if ((c_ctl == 'Z' || c_ctl == 'V') && c_scan == ' ') {
        int32_t half = (int32_t)(ctl->arg >> 1);

        v = self->trk_wr[0];
        Track_Decay(self, self->trk_buf[0], v, half, half,
                    (uint8_t)self->s3_param_def[0], 0x2e);
        v += half;
        w = (int32_t)ctl->arg - half;
        Track_Decay(self, self->trk_buf[0], v, w, w, 0, 0);
        self->s3_param[0].mode = 0;
    }

    v = self->trk_wr[17] - self->trk_rd[17];
    if (self->s3_param[17].shape_in > v)
        self->s3_param[17].shape_in = v;
    v = self->trk_wr[0] - self->trk_rd[0];
    if (self->s3_param[0].shape_in > v)
        self->s3_param[0].shape_in = v;

    if (c_ctl == ' ') {
        int32_t narrow = 0;

        if ((Phone_Attr(c_cur) & 4) && c_cur != 'Z' &&
            ctl->next->value != '&') {
            self->s3_param[17].mode |= 1;
            self->s3_param[17].lead = 0x3a;
            self->s3_param[0].lead = 0x30;
            self->s3_param[17].shape_in =
                (int32_t)st->cur->arg - (int32_t)(st->cur->arg / 3u);
            mode0 = self->s3_param[0].mode | 1;
            self->s3_param[0].mode = mode0;
            w = (int32_t)st->cur->arg - (int32_t)(st->cur->arg / 3u);
            self->s3_param[0].shape_in = w;

            if (c_scan == ' ') {
                if ((Phone_Attr(c_cur) & 0x10) ||
                    (Phone_Attr(c_cur | 0x180) & 1) ||
                    ((Phone_Attr(c_cur | 0x100) & 2) && c_cur != 'p') ||
                    (c_cur == 'p' && (Phone_Attr(back | 0x180) & 1)))
                    narrow = 1;
            }
            if (narrow) {
                self->s3_param[0].mode = mode0 & 5;
                self->s3_param[17].lead = 0x9b;
                if (Phone_Attr(c_cur | 0x100) & 2) {
                    self->s3_param[0].lead = 0x26;
                    self->s3_param[0].start = 0x26;
                } else {
                    self->s3_param[0].lead = 0x2d;
                    self->s3_param[0].start = 0x2d;
                }
                self->s3_param[0].target = 0;
                self->trk_wr[0] += 3;
                self->s3_param[0].len -= 3;
                self->s3_param[0].shape_out = 4;
                self->s3_param[0].shape_in = w + 3;
                if (c_cur == 'N') {
                    if (Phone_IsVowel((uint8_t)back)) {
                        self->s3_param[0].lead = 0x2a;
                        self->s3_param[0].shape_in = 8;
                    }
                    self->s3_param[9].mode = 6;
                    v = self->s3_param_def[9];
                    self->s3_param[9].start = v;
                    self->s3_param[9].target = v - 0xa;
                }
                if (c_cur == 'p') {
                    self->s3_param[1].mode = 4;
                    self->s3_param[17].shape_in =
                        (int32_t)st->cur->arg + 2;
                } else {
                    v = (int32_t)(st->cur->arg / 6u);
                    self->s3_param[17].shape_in = v;
                    if (v < 2)
                        self->s3_param[17].shape_in = 2;
                }
            }
        }

        i = c_cur - 0x56;
        switch (((uint32_t)i <= 0x24) ? g_op14_cur_sel[i] : 5) {
        case 0:
            self->s3_param[0].shape_out = 2;
            self->s3_param[1].shape_out = 2;
            self->s3_param[0].lead = 0;
            self->s3_param[0].start = 5;
            self->s3_param[0].shape_in = (int32_t)st->cur->arg - 4;
            break;
        case 1:
            self->s3_param[0].shape_out = 2;
            self->s3_param[1].shape_out = 2;
            self->s3_param[1].start = 0x14;
            self->s3_param[1].lead = 0x14;
            self->s3_param[1].shape_in = (int32_t)(st->cur->arg / 3u);
            self->s3_param[0].start = 0;
            self->s3_param[0].lead = 0;
            self->s3_param[2].shape_in = (int32_t)(st->cur->arg / 3u);
            break;
        case 2:
            if (c_scan == ' ') {
                self->s3_param[10].lead = 0x37c;
                self->s3_param[10].shape_in = 9;
                self->s3_param[10].mode |= 1;
                self->s3_param[0].mode |= 1;
                self->s3_param[0].shape_in = 8;
                self->s3_param[0].lead = self->s3_param_def[0];
            }
            break;
        case 3:
            self->s3_param[1].mode = 4;
            self->s3_param[1].start = 0;
            self->s3_param[1].target = 0;
            if (back == 'P' || back == 'T' || back == 'K') {
                self->s3_param[0].mode = 4;
                self->s3_param[0].lead = 0;
                self->s3_param[0].target = 0;
            }
            break;
        case 4:
            self->s3_param[0].shape_out = 2;
            self->s3_param[1].shape_out = 2;
            self->s3_param[1].shape_in = (int32_t)(st->cur->arg >> 1);
            break;
        default:
            break;
        }

        if (self->s3_param_def[0] == 0)
            self->s3_param[0].mode = 4;
    }

    if (c_cur == 'l' || c_cur == 'j') {
        cv = ctl->value;
        if (!(Phone_Attr(px3(cv) | 0x100) & 2) && cv != 'R' && cv != 'Y') {
            for (i = 0; i < 4; i++) {
                self->s3_param[9 + i].start = self->s3_param_def[9 + i];
                self->s3_param[9 + i].mode = 6;
            }
        }
    }

    if ((Phone_Attr(c_cur) & 0x20) && (back == 'M' || back == 'N')) {
        self->s3_param[16].shape_in = 2;
        self->s3_param[16].mode = 5;
        self->s3_param[16].lead = 0xfa;
    }

    if (c_ctl == 'T' || c_ctl == 'q') {
        if (st->cur->value == 'N' && !(st->cur->prev->flags & 0x20u) &&
            (Phone_Attr(back | 0x100) & 2)) {
            cv = ctl->next->value;
            if (cv == '%' || cv == '&') {
                int32_t ok = 0;

                if (Phone_Attr(px3(st->scan->value)) & 2)
                    ok = 1;
                else if ((Phone_Attr(px3(st->scan->value) | 0x100) & 2) &&
                         (st->scan->next->flags & 0x20u))
                    ok = 1;
                if (ok) {
                    self->s3_param[0].lead = 0x28;
                    self->s3_param[0].mode |= 1;
                    self->s3_param[0].shape_in = (int32_t)st->cur->arg;
                }
            }
        }
    }

    if (c_ctl == 'q') {
        cv = st->scan->value;
        if ((!(Phone_Attr(px3(cv)) & 4) || cv == 'x') &&
            (Phone_Attr(px3(st->cur->value) | 0x100) & 2)) {
            self->s3_param[17].mode |= 1;
            self->s3_param[17].lead = 0x28;
            v = self->s3_param_def[17];
            if (v >= 0x64)
                self->s3_param[17].lead = 0x14;
            if (v >= 0x96)
                self->s3_param[17].lead -= 0x14;
            self->s3_param[17].shape_in = 4;
        }
    }

    if (c_cur == 's')
        self->s3_param[1].mode = 4;

    if (c_ctl == 'V' && (Phone_Attr(c_cur | 0x280) & 0x40) &&
        (Phone_Attr(c_cur | 0x100) & 1) &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2)) {
        self->s3_param[1].mode = 6;
        self->s3_param[0].shape_out = 4;
        self->s3_param[1].shape_out = 4;
    }

    if (c_ctl == 'R' && c_cur == 'B' &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2))
        self->s3_param[0].mode = 4;

    if (c_ctl == 'Z' && (Phone_Attr(c_cur) & 0x10) &&
        (Phone_Attr(px3(st->scan->value) | 0x100) & 2)) {
        self->s3_param[0].target = 0x35;
        self->s3_param[0].mode = 4;
        self->s3_param[1].target = 0x40;
        self->s3_param[1].shape_out = 2;
        self->s3_param[2].target = 0;
        self->s3_param[4].target = 0;
        self->s3_param[18].target = 0xa;
        self->s3_param[19].target = 0xe;
        self->s3_param[10].target = 0x734;
        self->s3_param[11].target = 0x9c0;
        self->s3_param[13].target = 0x46;
        self->s3_param[5].target = 0x4a;
        self->s3_param[6].target = 0x4a;
        self->s3_param[7].target = 0x4a;
    }

    if (ctl->value == 'F' && st->cur->value == 'Z' &&
        st->cur->prev->value == 'E' && ctl->next->value == 'k')
        self->s3_param[1].target = 0x37;
}

/* Which rule the phoneme just finished calls for. */
/* @0x1004e2f8 */ extern const uint8_t g_op13_sel[0x31];
/* And which one the phoneme coming up calls for. */
/* @0x1004e348 */ extern const uint8_t g_op13_sel2[0x13];
/* The highest the fourth formant may go, per voice. */
/* @0x100b53f0 */ extern const int32_t g_voice_f4max[];

/*
 * The first parameter pass.
 *
 * This is where the formants of the phoneme coming up are settled: the
 * glide tables are consulted for the pairs that have one, the four formants
 * are eased from where they are towards where the tables put them, and then
 * a long list of named pairs overrides what that produced.  The last part
 * looks after the pitch track across a phrase-final fall.
 */
/* @0x1004bfc0 */
void TV_THISCALL Stage3_Op13(Engine *self)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *scan = st->scan;
    Node *n;
    S3Param *p = self->s3_param;
    int32_t c_ctl = px3(ctl->value);
    int32_t c_cur = px3(cur->value);
    int32_t back;
    uint8_t a, cl, dl;
    int32_t i, k, v, w;

    n = cur->prev;
    back = px3(n->value);
    if (Phone_Attr(back) & 8) {
        n = n->prev;
        back = (n != NULL) ? px3(n->value) : 0x20;
    }

    if (c_ctl == 't') {
        for (i = 0; i < 3; i++)
            p[9 + i].target = Synth_MulQ15(self->s3_param_def[9 + i] +
                                           self->s3_next[9 + i] +
                                           p[9 + i].target, 0x2a48);
    }

    if ((Phone_Attr(c_cur | 0x180) & 8) &&
        !(Phone_Attr(c_ctl) & 0x10) &&
        !(Phone_Attr(c_ctl | 0x200) & 0x10) &&
        !(Phone_Attr(c_ctl | 0x100) & 0x90) &&
        !Phone_IsVowel((uint8_t)c_ctl))
        p[10].shape_out -= 2;

    self->s3_2035 = 0;
    cl = ctl->value;
    if (cl == 'P' || cl == 'T' || cl == 'K' || cl == 'B' || cl == 'D' ||
        cl == 'G' || (Phone_Attr(px3(cl)) & 0x40) || cl == 'H' ||
        cl == 'd' ||
        (cl == 'R' && cur->value != 's') ||
        (cl == 'L' && (Phone_Attr(px3(ctl->prev->value)) & 0x20)) ||
        (cl == 'Y' && (Phone_Attr(px3(cur->value) | 0x100) & 2) &&
         (Phone_Attr(px3(ctl->next->value) | 0x100) & 2))) {
        v = Vowel_Index(scan->value);
        if (v != -1) {
            self->s3_2035 = Stage3_GlideTab(self, v);
        } else if ((scan->value == 'R' && c_ctl != 'P' && c_ctl != 's') ||
                   ((Phone_Attr(c_ctl) & 0x20) &&
                    ctl->next->value == 'L')) {
            Stage3_Formants(self, 1);
        }
    }

    if (c_ctl == 'T' && ctl->next->value == 'o') {
        p[1].target = 0x44;
        p[3].target = 0x32;
        p[4].target = 0x32;
        p[5].target = 0x46;
        p[6].target = 0x46;
    }
    if (ctl->value == 'K') {
        a = scan->value;
        if (a == 'r' || a == 'g' || a == 'c') {
            p[10].target = 0x438;
            p[11].target = 0x690;
            p[12].target = 0xb50;
            p[1].target = 0x3e;
            p[3].target = 0x41;
            p[15].target = 0xc8;
            p[14].target = 0xc8;
        }
        if (scan->value == 'g') {
            p[1].target = 0x41;
            p[2].target = 0x37;
            p[6].target = 0x46;
        }
    }
    if (c_cur == 'K' && (c_ctl == 'r' || c_ctl == 'g' || c_ctl == 'c')) {
        p[3].target = 0x3c;
        p[4].target = 0x3c;
        p[5].target = 0x3c;
    }
    if (ctl->value == 'F')
        p[9].target = 0x122;
    a = ctl->value;
    if ((a == 'P' || a == 'T' || a == 'K') && self->s3_2035 != 0 &&
        p[9].target > 0x1f4)
        p[9].target = 0x1f4;
    if (ctl->value == 'K' && scan->value == '|') {
        p[10].target = 0x6ec;
        p[11].target = 0x8c0;
    }

    if (p[0].target > 0 || p[2].target > 0) {
        v = g_voice_f4max[st->voice];
        if (p[12].target > v)
            p[12].target = v;
        for (i = 0; i < 3; i++)
            if (p[10 + i].target < p[9 + i].target + 0xc8)
                p[10 + i].target = p[9 + i].target + 0xc8;
    }

    for (i = 0; i < 0x16; i++) {
        if (self->s3_2034 != 0 && i >= 9 && i <= 0xf)
            continue;
        if (i == 0x10)
            continue;
        if (p[i].shape_out > self->s3_1fe8)
            p[i].shape_out = self->s3_1fe8;
        p[i].shape_in = p[i].shape_out;
        if (p[i].len > 0x3c)
            p[i].len = 0x3c;
    }

    k = (self->s3_2034 != 0 && (Phone_Attr(c_ctl | 0x100) & 2)) ? 0x10 : 9;
    for (; k <= 0x11; k++) {
        if (c_cur == 'N' && (Phone_Attr(c_ctl | 0x100) & 2) &&
            k >= 0xa && k <= 0xf)
            continue;
        v = Synth_MulQ15(self->s3_param_def[k],
                         0x7ffe - self->s3_param_rate[k]) +
            Synth_MulQ15(p[k].target, self->s3_param_rate[k]);
        p[k].start = v;
        p[k].lead = v;
    }

    if ((c_cur == 'T' || c_cur == 'D' || c_cur == 'q') && c_ctl == 'W') {
        p[10].lead = 0x4b0;
        p[10].start = 0x4b0;
        p[11].lead = 0x802;
        p[11].start = 0x802;
        p[12].shape_out = 3;
        p[12].lead = 0x9c4;
        p[12].start = 0x9c4;
    }

    i = c_ctl - 0x43;
    switch (((uint32_t)i <= 0x30) ? g_op13_sel[i] : 11) {
    case 0:
        if (ctl->next->value == 's' &&
            ctl->next->next->value == ' ') {
            p[10].target = 0x734;
            p[11].target = 0x9e0;
        }
        /* fall through */
    case 10:
        if (c_cur != 'C')
            break;
        if (ctl->next->value == ' ') {
            p[10].target = 0x734;
            p[11].target = 0x9e0;
            p[4].target = 0x46;
            p[3].target = 0x3c;
            p[1].target = 0x3c;
        } else if (Phone_IsVowel((uint8_t)back) &&
                   !(Phone_Attr(px3(scan->value) | 0x100) & 2) &&
                   scan->value != 'R') {
            for (i = 0; i < 4; i++) {
                p[9 + i].target = self->s3_param_def[9 + i];
                p[9 + i].mode = 4;
            }
        }
        break;
    case 1:
        if (Phone_IsVowel((uint8_t)c_cur)) {
            for (i = 0; i < 4; i++) {
                p[9 + i].start = self->s3_param_def[9 + i];
                p[9 + i].mode = 6;
            }
        }
        break;
    case 2:
        if (c_cur == ' ') {
            p[8].shape_out = 7;
            p[1].shape_out = 7;
        }
        if (Phone_Attr(px3(scan->value) | 0x100) & 2) {
            p[3].target = 0;
            p[1].target = 0x3c;
        }
        break;
    case 3:
        if (scan->value == 'p')
            p[10].shape_in = 7;
        if (c_cur != ' ' && scan->value != 'Y') {
            v = (int32_t)(cur->arg - (cur->arg >> 2));
            p[11].shape_in = v;
            p[10].shape_in = v;
        }
        a = scan->value;
        if (a != 'o') {
            if ((Phone_Attr(px3(a) | 0x100) & 0x20) ||
                !(Phone_Attr(px3(a) | 0x200) & 1))
                p[0].shape_out = 8;
        }
        if (scan->value == 'l') {
            p[9].target = 0x142;
            p[10].target = 0x4b5;
            p[11].target = 0x9e3;
            p[12].target = 0xe03;
        }
        break;
    case 4:
        if (!(Phone_Attr(c_cur) & 4))
            p[0].shape_out = 8;
        break;
    case 5:
        if (Phone_IsVowel((uint8_t)c_cur)) {
            p[9].mode = 6;
            p[9].start = self->s3_param_def[9];
        }
        if (c_cur == 'E')
            p[10].target = 0x4a1;
        break;
    case 6:
        if (scan->value == 'E') {
            a = ctl->prev->value;
            if (a == '&' || a == '%' || a == ' ') {
                p[1].target = 0x37;
                p[3].target = 0x3e;
                p[6].target = 0;
            }
        }
        break;
    case 7:
        a = scan->value;
        if (a != 'E' && a != 'e')
            break;
        for (i = 3; i < 5; i++)
            if (p[i].target > 3)
                p[i].target -= 3;
        if (scan->value == 'e') {
            p[3].target = 0x32;
            p[7].target = 0;
            p[6].target = 0;
            p[5].target = 0x41;
        }
        if (scan->value == 'E') {
            p[1].target = 0x3e;
            p[5].target = 0x41;
            p[4].target = 0x41;
            p[7].target = 0x44;
            p[6].target = 0x44;
            a = ctl->prev->value;
            if (a == '&' || a == '%' || a == ' ') {
                p[2].target = 0x3c;
                p[4].target = 0;
                p[5].target = 0x3c;
                p[8].target = 0;
                p[1].target = 0x41;
                p[6].target = 0x4d;
                p[7].target = 0x4d;
            }
        }
        break;
    case 8:
        if (Phone_Attr(px3(scan->value) | 0x100) & 2) {
            for (i = 9; i < 13; i++)
                p[i].shape_out = 3;
        }
        break;
    case 9:
        if (c_cur == 'O' && ctl->next->value == 'D') {
            p[9].target = 0x178;
            p[10].target = 0x25c;
            p[12].target = 0xea0;
            p[10].shape_out = 3;
            p[12].shape_out = 3;
        }
        break;
    default:
        break;
    }

    if (Phone_Attr(c_cur) & 0x10) {
        if (c_cur == 'M') {
            int32_t alt = 0;

            if (Phone_Attr(c_ctl | 0x100) & 2) {
                if (!Phone_IsVowel((uint8_t)c_ctl))
                    alt = 1;
                else if (!(ctl->flags & 0x20u) &&
                         (Phone_Attr(px3(scan->value)) & 0x10))
                    alt = 1;
            }
            if (!alt && (Phone_Attr(c_ctl | 0x80) & 0x20))
                alt = 1;
            if (alt) {
                p[16].start = 0x17c;
                p[16].shape_out = 4;
                p[16].mode = 6;
            } else {
                p[16].mode = 4;
            }

            if (Phone_IsVowel((uint8_t)c_ctl)) {
                p[9].mode = 1;
                p[9].lead = p[9].start;
            } else if (Phone_IsVowel((uint8_t)back)) {
                for (i = 0; i < 3; i++) {
                    p[9 + i].lead = p[9 + i].start;
                    p[9 + i].mode |= 1;
                    p[9 + i].shape_in = 3;
                }
            } else {
                p[9].start = self->s3_param_def[9];
                if (Phone_Attr(c_ctl) & 2) {
                    p[9].mode |= 1;
                    p[12].mode |= 1;
                    p[10].mode &= 7;
                    if (c_ctl == '4')
                        p[10].mode &= 6;
                    p[11].mode &= 7;
                    for (i = 13; i < 16; i++)
                        p[i].mode &= 6;
                }
            }
        }

        if (c_cur == 'N') {
            if (Phone_Attr(c_ctl) & 0x20) {
                p[16].mode = 4;
            } else {
                int32_t big = 0;

                cl = (uint8_t)(Phone_Attr(c_ctl | 0x100) & 2);
                if (cl == 0)
                    big = 1;
                else if (!(ctl->flags & 0x20u) &&
                         (Phone_Attr(px3(scan->value)) & 0x10))
                    big = 1;
                else if (c_ctl == '|' &&
                         (Phone_Attr(back | 0x100) & 2))
                    big = 1;
                if (big) {
                    p[16].shape_out = 4;
                    p[16].mode = 6;
                    p[16].start = 0x190;
                } else if (cl) {
                    p[16].shape_out = 4;
                    p[16].mode = 6;
                } else {
                    p[16].mode = 4;
                }
            }
            if ((Phone_Attr(c_ctl) & 2) &&
                !Phone_IsVowel((uint8_t)c_ctl)) {
                p[12].mode |= 1;
                p[9].shape_in = 3;
                p[12].shape_in = 4;
                for (i = 10; i < 16; i++)
                    p[i].mode &= 6;
            }
            if (!Phone_IsVowel((uint8_t)c_ctl)) {
                p[9].mode |= 1;
                p[9].lead = p[9].start;
            }
            if (c_ctl == 'X')
                p[9].mode |= 6;
            if (Phone_Attr(c_ctl | 0x80) & 0x20)
                p[10].mode &= 6;
        }

        if (c_cur == 'n') {
            if (!Phone_IsVowel((uint8_t)c_ctl)) {
                p[11].start = 0xa28;
                p[12].start = 0xe10;
            }
            if ((Phone_Attr(c_cur | 0x200) & 1) &&
                !(Phone_Attr(c_cur | 0x100) & 0x20) &&
                !Phone_IsVowel((uint8_t)c_ctl))
                p[10].start = 0x686;
        }

        if (Phone_IsVowel((uint8_t)back) && cur->arg < 6u) {
            for (i = 0; i < 4; i++) {
                p[9 + i].mode |= 1;
                p[9 + i].shape_in = (int32_t)cur->arg;
            }
        }
        if (Phone_Attr(c_ctl | 0x180) & 1) {
            p[16].mode = 6;
            p[16].start = 0x17c;
            p[16].shape_out = 4;
        }
        if (Phone_Attr(c_ctl | 0x100) & 2) {
            a = ctl->prev->prev->value;
            if (a == ' ' || a == '%' || a == '&' || a == 0 ||
                (Phone_Attr(px3(a) | 0x100) & 2))
                p[10].mode = 0;
        }
    }

    if (Phone_Attr(c_ctl) & 0x10) {
        if (c_ctl == 'M' || c_ctl == 'N') {
            p[11].mode = 4;
            p[10].mode = 4;
            p[16].mode = 5;
            p[16].shape_in = 4;
        }
        if (c_ctl == 'M') {
            p[12].mode = 6;
            p[12].start = self->s3_param_def[12];
        }
        if (c_ctl == 'N') {
            if (Phone_IsVowel(cur->value)) {
                p[12].mode = 6;
                p[12].start = self->s3_param_def[12];
            }
            a = scan->value;
            if (a == 'e') {
                p[10].start = 0x32c;
                p[10].mode = 6;
                p[10].shape_out = p[10].len;
            } else if (a == 'b') {
                p[11].start = 0x9aa;
                p[11].mode = 6;
                p[11].shape_out = p[11].len;
            }
        }
        a = Phone_Attr(px3(scan->value));
        if ((a & 0x20) && !(a & 4)) {
            p[9].mode = 6;
            p[9].shape_out = 2;
        }
        if ((Phone_Attr(c_ctl) & 0x10) && Phone_IsVowel((uint8_t)c_cur) &&
            Phone_IsVowel(scan->value) && ctl->arg < 6u) {
            for (i = 0; i < 4; i++) {
                p[9 + i].target = self->s3_param_def[9 + i];
                p[9 + i].mode = 4;
            }
        }
        if (c_ctl == 'N' &&
            (c_cur == ' ' || (Phone_Attr(c_cur | 0x100) & 2)) &&
            (Phone_Attr(px3(scan->value) | 0x100) & 2)) {
            p[10].target = 0x36c;
            p[10].mode = 4;
            self->trk_rd[10] = self->trk_wr[10];
        }
        if (c_ctl == 'N' || c_ctl == '~') {
            int32_t go = 0;

            if (Phone_IsVowel(cur->value))
                go = 1;
            else if (Phone_Attr(px3(cur->value)) & 0x40)
                go = 1;
            else if ((Phone_Attr(px3(cur->value) | 0x100) & 1) &&
                     Phone_IsVowel(scan->value))
                go = 1;
            if (go) {
                v = self->s3_param_def[9];
                p[9].start = v;
                v -= (ctl->arg > 4u) ? 0x64 : 0x32;
                p[9].target = v;
                if (p[9].target < 0xfa)
                    p[9].target = 0xfa;
                p[9].mode = 6;
                p[9].shape_out = (int32_t)(ctl->arg >> 1);
            }
        }
    }

    if (c_ctl == 'R') {
        if (scan->value == 'E')
            p[0].shape_out = 5;
        if (c_cur == 'X')
            p[11].shape_in = 3;
        if (Phone_IsVowel((uint8_t)c_cur)) {
            p[9].start = self->s3_param_def[9] - 0xa;
            p[10].start = self->s3_param_def[10] - 0x14;
            p[12].target = self->s3_param_def[12];
            p[12].mode = 4;
            p[11].start = self->s3_param_def[11] - 0x28;
            for (i = 9; i < 12; i++) {
                p[i].mode = 6;
                p[i].shape_out = (int32_t)(ctl->arg >> 1);
            }
            if (Phone_Attr(px3(scan->value) | 0x100) & 2) {
                Stage3_Formants(self, 0);
            } else if (self->s3_2035 != 0) {
                p[9].target -= 0x14;
                if (p[9].start < p[9].target)
                    p[9].target = p[9].start - 0xa;
                p[10].target -= 0x28;
                if (p[10].start < p[10].target)
                    p[10].target = p[10].start - 0x28;
                p[11].target -= 0x6e;
                if (p[11].start < p[11].target)
                    p[11].target = p[11].start - 0x3c;
                v = p[10].target - p[9].target;
                if (v < 0xc8)
                    p[10].target = p[10].target - v + 0xc8;
                v = p[11].target - p[10].target;
                if (v < 0xc8)
                    p[11].target = p[11].target - v + 0xc8;
            } else {
                p[9].target = self->s3_param_def[9] - 0x14;
                p[10].target = self->s3_param_def[10] - 0x28;
                p[11].target = self->s3_param_def[11] - 0x6e;
            }
            if (c_cur == ' ') {
                p[11].mode = 5;
                p[10].mode = 5;
            }
        } else {
            cl = Phone_Attr(c_cur | 0x100);
            if (cl & 2) {
                for (i = 0; i < 4; i++) {
                    p[9 + i].start = self->s3_param_def[9 + i];
                    p[9 + i].mode = 6;
                }
            } else if (((Phone_Attr(c_cur | 0x80) & 0x20) &&
                        c_cur != 's') ||
                       c_cur == 'l' || c_cur == 'n' || c_cur == ' ') {
                Stage3_Formants(self, 0);
            } else if (cl & 1) {
                for (i = 0; i < 4; i++) {
                    p[9 + i].start = self->s3_param_def[9 + i];
                    p[9 + i].mode = 6;
                    p[9 + i].shape_out = (int32_t)ctl->arg;
                }
                if (Phone_IsVowel(scan->value)) {
                    p[9].mode = 4;
                    p[9].target = self->s3_param_def[9];
                    if (!(Phone_Attr(c_cur) & 4)) {
                        p[12].mode = 4;
                        p[12].target = self->s3_param_def[12];
                    }
                }
            } else if (Phone_Attr(c_cur) & 0x40) {
                for (i = 9; i < 13; i++) {
                    p[i].mode = 7;
                    p[i].shape_in = 3;
                }
                if (Phone_IsVowel(scan->value) &&
                    !(Phone_Attr(c_cur) & 4)) {
                    p[12].mode = 4;
                    p[12].target = self->s3_param_def[12];
                }
            } else {
                p[9].mode = 6;
                p[9].start = self->s3_param_def[9];
                p[9].shape_out = (int32_t)ctl->arg;
                for (i = 10; i < 13; i++)
                    p[i].mode = 4;
            }
        }

        if ((Phone_Attr(px3(scan->value) | 0x100) & 2) &&
            !(scan->flags & 0x20u)) {
            a = ctl->prev->value;
            if (a != '&' && a != '%' && cur->value == 'T') {
                for (i = 0; i < 4; i++) {
                    p[9 + i].target = self->s3_param_def[9 + i];
                    p[9 + i].mode = 4;
                }
            }
        }
    }

    if (c_ctl == 'L' && (Phone_Attr(px3(ctl->prev->value)) & 0x20))
        Stage3_Formants(self, 0);
    if (c_ctl == 'Y' &&
        (Phone_Attr(px3(ctl->next->value) | 0x100) & 2) &&
        (Phone_Attr(px3(cur->value) | 0x100) & 2) &&
        !(Phone_Attr(px3(cur->value) | 0x180) & 0x40))
        Stage3_Formants(self, 0);

    if ((Phone_Attr(c_ctl) & 0x40) && (Phone_Attr(c_cur | 0x100) & 2)) {
        p[0].shape_in = 5;
        if ((Phone_Attr(c_ctl | 0x200) & 0x10) ||
            (Phone_Attr(c_ctl | 0x100) & 0x10)) {
            p[11].shape_in = 7;
            p[10].shape_in = 7;
            p[0].shape_out = 4;
            p[11].shape_out = 2;
            p[10].shape_out = 2;
        } else if (Phone_Attr(c_ctl | 0x100) & 4) {
            p[11].shape_out = 2;
            p[11].shape_in = 6;
            p[10].shape_in = 6;
            p[10].shape_out = 2;
        }
        if (Phone_IsVowel((uint8_t)c_cur) && c_ctl == 'S') {
            for (i = 0; i < 4; i++)
                p[9 + i].start = self->s3_param_def[9 + i];
        }
        if (Phone_IsVowel((uint8_t)c_cur)) {
            if (c_ctl != 'F')
                goto tail_f;
            if (Phone_IsVowel(scan->value)) {
                for (i = 0; i < 4; i++) {
                    p[9 + i].start = self->s3_param_def[9 + i];
                    p[9 + i].shape_out = (int32_t)ctl->arg;
                }
                for (i = 4; i < 9; i++)
                    p[i].target = 0x3c;
            }
        }
    } else if (Phone_Attr(c_cur) & 0x40) {
        cl = Phone_Attr(c_cur);
        if ((Phone_Attr(c_ctl | 0x100) & 2) && self->s3_2034 == 0) {
            if ((Phone_Attr(c_cur | 0x200) & 0x10) ||
                (Phone_Attr(c_cur | 0x100) & 0x10)) {
                if (cl & 4)
                    p[0].shape_out = 5;
            } else if (Phone_Attr(c_cur | 0x100) & 4) {
                for (i = 3; i < 8; i++)
                    p[i].shape_out = 7;
            }
            if (c_cur == 's')
                p[1].shape_out = 5;
        }
        for (i = 9; i < 13; i++) {
            p[i].lead = p[i].start;
            v = (int32_t)(cur->arg - (cur->arg >> 2));
            p[i].shape_in = v;
            if (c_cur == 's')
                p[i].shape_in = v + (int32_t)(cur->arg >> 2);
            if (c_ctl == 'R' &&
                ((c_cur == 'z' && self->s3_1fed == 'J') ||
                 (c_cur == 's' && self->s3_1fed == 'C')))
                p[i].shape_in = (int32_t)cur->arg;
        }
    }

    if (c_ctl == 'F') {
        a = scan->value;
        if (a == '4' || a == 'k' || a == 'U') {
            p[8].target = 0x41;
            p[4].target = 0x23;
            p[5].target = 0x23;
        }
    }

tail_f:
    if (ctl->value == 'F') {
        cl = Phone_Attr(px3(scan->value) | 0x100);
        if ((cl & 2) && (Phone_Attr(px3(scan->value) | 0x200) & 1) &&
            (cl & 0x20))
            p[8].target = 0x3c;
        if (cur->value == 'T' && scan->value == 'U')
            p[8].target = 0x37;
    }

    if ((Phone_Attr(c_cur) & 0x20) &&
        !(self->s3_2034 != 0 && (Phone_Attr(c_ctl | 0x100) & 2))) {
        i = c_cur - 0x42;
        switch (((uint32_t)i <= 0x12) ? g_op13_sel2[i] : 6) {
        case 0:
        case 4:
            p[10].mode &= ~1;
            p[11].mode &= ~1;
            p[0].shape_out = 2;
            if ((c_cur == 'B' && (Phone_Attr(c_ctl | 0x100) & 2)) ||
                (c_cur == 'P' && c_ctl == 'o')) {
                p[11].shape_out = 3;
                p[10].shape_out = 3;
                p[9].shape_out = 3;
            }
            break;
        case 1:
            a = Phone_Attr(c_ctl | 0x100);
            if ((a & 0x20) || c_ctl == 'o' || c_ctl == 'u' ||
                c_ctl == 'b') {
                p[11].shape_out = 5;
                p[10].shape_out = 5;
                if (a & 0x20) {
                    p[11].shape_in = 0;
                    p[10].shape_in = 0;
                }
                if (c_ctl == 'o' || c_ctl == 'u' || c_ctl == 'b')
                    p[0].shape_out = 4;
                if (g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]] == 0)
                    p[10].start = 0x784;
            }
            break;
        case 2:
            p[11].shape_in = 0;
            p[0].shape_out = 5;
            p[10].shape_in = 0;
            v = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]];
            if (v == 2 || v == 3) {
                for (i = 9; i < 13; i++)
                    p[i].shape_out = 5;
            } else {
                p[11].shape_out = 7;
                p[10].shape_out = 7;
            }
            if (c_ctl == 'o' || c_ctl == 'E')
                p[0].shape_out = 4;
            break;
        case 3:
            p[0].shape_out = 3;
            if (c_ctl == 'u')
                p[11].shape_out = 6;
            break;
        case 5:
            v = (int32_t)g_class_kind[TV_REF(uint8_t, g_phone_class)[c_ctl]];
            if ((v == 0 || v == 1) &&
                (Phone_Attr(c_ctl | 0x100) & 0x20) &&
                (Phone_Attr(c_ctl | 0x200) & 1) && c_ctl != 'E') {
                p[11].lead = 0xa8c;
                p[11].start = 0xa8c;
            }
            break;
        default:
            break;
        }
    }

    if (c_cur == 'R') {
        if (Phone_IsVowel((uint8_t)c_ctl) &&
            (Phone_IsVowel((uint8_t)back) ||
             (Phone_Attr(back) & 0x40) ||
             (Phone_Attr(back | 0x100) & 1))) {
            p[12].mode = 1;
            p[12].shape_in = (int32_t)cur->arg + 1;
        }
        if ((Phone_Attr(c_ctl | 0x100) & 2) && !(ctl->flags & 0x20u) &&
            back == 'T') {
            for (i = 9; i < 13; i++) {
                p[i].mode = 1;
                p[i].shape_in = (int32_t)cur->arg + 1;
            }
        }
    }

    if ((Phone_Attr(c_ctl | 0x100) & 1) && !(Phone_Attr(c_ctl) & 0x10)) {
        p[9].mode = 6;
        p[9].start = self->s3_param_def[9];
        p[9].lead = self->s3_param_def[9];
        v = self->s3_1e3c;
        if (v > 0 && v != 0x7f)
            p[9].shape_out = p[9].len - v;
        else
            p[9].shape_out = p[9].len;
        if (Phone_IsVowel((uint8_t)c_cur)) {
            a = ctl->prev->value;
            if (!((a == '&' || a == '%') &&
                  (Phone_Attr(c_cur | 0x180) & 0x40))) {
                for (i = 10; i < 13; i++) {
                    p[i].start = self->s3_param_def[i];
                    p[i].lead = self->s3_param_def[i];
                    p[i].mode = 6;
                    p[i].shape_out = p[i].len;
                }
            }
        }
        if (c_cur == 'j' && Phone_IsVowel((uint8_t)back)) {
            for (i = 9; i < 13; i++) {
                p[i].start = self->s3_param_def[i];
                p[i].lead = self->s3_param_def[i];
                p[i].mode = 6;
                p[i].shape_out = p[i].len;
            }
        }
    }

    cl = ctl->value;
    if (cl == 'P' || cl == 'T' || cl == 'K' || cl == 'B' || cl == 'D' ||
        cl == 'G' || (Phone_Attr(px3(cl)) & 0x40) || cl == 'H' ||
        cl == 'd') {
        dl = scan->value;
        if (((Phone_Attr(px3(dl) | 0x100) & 2) || dl == 'R') &&
            cl != 'F') {
            for (i = 9; i < 13; i++) {
                p[i].start = self->s3_param_def[i];
                p[i].lead = self->s3_param_def[i];
                p[i].mode = 6;
                p[i].shape_out = p[i].len;
            }
        }
    }

    if (c_cur == 'M' &&
        (Phone_Attr(px3(self->s3_1fed) | 0x100) & 2)) {
        if ((Phone_Attr(c_ctl | 0x100) & 2) && self->s3_2034 != 0) {
            p[9].mode = 0x10;
            p[9].shape_out = 0;
            p[9].shape_in = (int32_t)(cur->arg - (cur->arg >> 1)) + 1;
        } else {
            p[9].mode = 0x14;
            p[9].shape_in = (int32_t)(cur->arg - (cur->arg >> 1)) + 1;
            p[9].shape_out = p[9].len - 1;
        }
    }

    if (ctl->value == 'X' &&
        (Phone_Attr(px3(ctl->prev->value) | 0x100) & 2)) {
        dl = ctl->next->value;
        if ((dl == '&' || dl == '%') &&
            (Phone_Attr(px3(ctl->next->next->value) | 0x100) & 2)) {
            p[9].target = 0x128;
            p[10].target = 0x60c;
            p[8].target = 0x37;
            p[9].shape_out = 3;
            p[10].shape_out = 3;
        }
    }

    if (ctl->value == 'Y' &&
        (Phone_Attr(px3(ctl->prev->value) | 0x100) & 2) &&
        (Phone_Attr(px3(ctl->next->value) | 0x100) & 2)) {
        Track_RampTo(self->trk_buf[9], self->trk_wr[9], p[9].len + 1,
                     (uint8_t)(self->s3_param_def[9] >> 2),
                     (uint8_t)(p[9].target >> 2));
        p[9].mode = 0;
    }

    p[17].shape_out = p[17].len;
    if (p[17].shape_out < 7)
        p[17].shape_out = 7;
    if (p[17].shape_out > self->s3_1fe8)
        p[17].shape_out = self->s3_1fe8;

    if ((Phone_Attr(c_ctl | 0x100) & 2) && (ctl->flags & 0x20u) &&
        (Phone_Attr(c_cur | 0x80) & 0x20)) {
        p[17].start = self->s3_param_def[17];
        p[17].lead = self->s3_param_def[17];
    }

    if ((Phone_Attr(px3(ctl->value)) & 1) && (ctl->flags & 0x20u) &&
        !(ctl->flags & 0x40u) && (cur->flags & 0x20u) &&
        !(cur->flags & 0x40u) && (Phone_Attr(c_cur) & 4)) {
        p[17].shape_in = (int32_t)cur->arg;
        if (self->s3_1fe0 < (int32_t)cur->arg)
            p[17].shape_in = self->s3_1fe0;
    } else {
        p[17].shape_in = 0;
    }

    if (Phone_IsVowel((uint8_t)c_cur)) {
        for (i = 13; i < 16; i++) {
            p[i].lead = p[i].start;
            p[i].shape_in = (int32_t)(cur->arg - (cur->arg >> 2));
        }
    }

    if (ctl->flags & 0x40u) {
        if (self->s3_param_def[17] > 0) {
            v = (int32_t)(cur->arg / 3u);
            Track_BlendBack(self, self->trk_buf[17], self->trk_wr[17], v,
                            v, 0x29);
        }
        Track_Fill(self->trk_buf[17], self->trk_wr[17], 1, 0x14);
        self->trk_wr[17]++;
        v = p[17].start;
        Track_Fill(self->trk_buf[17], self->trk_wr[17], 1,
                   (uint8_t)((v - v / 4) >> 1));
        self->trk_wr[17]++;
        Track_Fill(self->trk_buf[17], self->trk_wr[17], 1,
                   (uint8_t)(p[17].start >> 1));
        self->trk_wr[17]++;
        p[17].len -= 3;
        self->s3_param_def[17] = p[17].start;
    }

    if ((Phone_Attr(c_ctl | 0x100) & 2) && (ctl->flags & 0x20u) &&
        scan->next->value == ' ' &&
        (scan->value == ' ' || scan->next->next->value == ' ')) {
        int32_t save_tgt = p[17].target;

        if (save_tgt < p[17].start) {
            if (ctl->arg > 0xau &&
                ((Phone_Attr(c_cur) & 4) || c_cur == ' ') &&
                !(ctl->flags & 0x40u)) {
                w = p[17].len - 3;
                p[17].shape_in = 0;
                p[17].len = 3;
                p[17].shape_out = 3;
                p[17].target = p[17].start + 4;
                Stage3_Write(self, 0x11);
                p[17].len = w;
                p[17].start = p[17].target;
                p[17].target = save_tgt;
                p[17].shape_out = w;
                p[17].shape_in = 0;
                self->trk_wr[17] += 3;
            }

            if (ctl->flags & 0x40u) {
                v = p[17].start / 4;
                if (v > 0xa)
                    v = 0xa;
                p[17].start += v;
                p[17].lead += p[17].lead / 4;
                k = p[17].len / 3;
            } else {
                k = 2;
            }
            Track_Fill(self->trk_buf[17], self->trk_wr[17], k,
                       (uint8_t)(p[17].start >> 1));
            w = p[17].len - k;
            self->trk_wr[17] += k;
            p[17].len = w;
            self->s3_param_def[17] = p[17].start;

            if (ctl->arg > 0xau &&
                ((Phone_Attr(c_cur) & 4) || c_cur == ' ') &&
                !(ctl->flags & 0x40u)) {
                int32_t save2 = p[17].target;

                p[17].shape_in = 0;
                p[17].len = 3;
                p[17].target = p[17].start - 4;
                p[17].shape_out = 3;
                w -= 3;
                Stage3_Write(self, 0x11);
                p[17].len = w;
                self->s3_param_def[17] = p[17].target;
                p[17].start = p[17].target;
                p[17].target = save2;
                p[17].shape_out = w;
                self->trk_wr[17] += 3;
            }
        }
    }

    if ((Phone_Attr(c_ctl | 0x100) & 2) &&
        (Phone_Attr(c_ctl | 0x180) & 0x40) && !(ctl->flags & 0x20u) &&
        scan->value == ' ' && p[17].target < 0x3e)
        p[17].target = 0x3e;

    if (c_ctl == 'T' && scan->value == 'p') {
        cl = Phone_Attr(px3(cur->value) | 0x100);
        if ((cl & 2) || (Phone_Attr(px3(cur->value)) & 0x10) ||
            (cl & 0x40)) {
            p[17].lead = 0x28;
            if (cur->arg < 0xau)
                p[17].lead = 0x14;
            p[17].mode |= 1;
            p[17].shape_in = (int32_t)(cur->arg >> 1) + 1;
        }
    }

    if (c_ctl == ' ' && c_cur == 's') {
        p[1].lead = 0x19;
        p[1].start = 0xf;
        p[1].shape_in = 9;
        p[1].shape_out = 3;
    }

    if (Phone_IsVowel((uint8_t)back) || back == '%' || back == '&') {
        if (!(cur->value == 'Y' && (Phone_Attr(back | 0x180) & 0x40))) {
            for (i = 9; i < 13; i++)
                if ((uint32_t)p[i].shape_in > cur->arg)
                    p[i].shape_in = (int32_t)cur->arg;
        }
    }
}

/*
 * Lay one formant along a path of five points.
 *
 * A reduced vowel does not hold a steady formant: it passes through several
 * values on its way, so this takes five of them with the frame each is
 * reached at and draws the whole run in one go -- the frames still owed to
 * the phoneme before, then a ramp per segment, then the bandwidth, which
 * only needs three.
 */
/* @0x10039d10 */
void TV_THISCALL Stage3_LayPath(Engine *self, int32_t v1, int32_t v2,
                                int32_t v3, int32_t v4, int32_t t1,
                                int32_t t2, int32_t t3, int32_t t4,
                                int32_t bw1, int32_t v5, int32_t bw2,
                                int32_t v0, int32_t bw0, int32_t param,
                                int32_t force)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *cur = st->cur;
    Node *ctl = st->ctl;
    uint8_t c_cur = cur->value;
    uint8_t b12 = 0, b13 = 0;
    uint8_t moved = 0;
    int32_t sv0 = 0, sv1 = 0, sv2 = 0, sv3 = 0, sv4 = 0, sv5 = 0;
    int32_t pos, n, k, m, half, quarter, rest;
    int32_t tmp14 = 0;

    switch (param) {
    case 9:
        b13 = (uint8_t)(self->s3_param_def[param] >> 2);
        b12 = (uint8_t)(self->s3_param_def[param + 4] >> 1);
        sv0 = (v0 & 0xff) << 2;
        sv1 = (v1 & 0xff) << 2;
        sv2 = (v2 & 0xff) << 2;
        sv3 = (v3 & 0xff) << 2;
        sv4 = (v4 & 0xff) << 2;
        sv5 = (v5 & 0xff) << 2;
        break;
    case 10:
        b13 = (uint8_t)((self->s3_param_def[param] - 0x1f4) >> 3);
        b12 = (uint8_t)(self->s3_param_def[param + 4] >> 1);
        sv0 = ((v0 & 0xff) * 8) + 0x1f4;
        sv1 = ((v1 & 0xff) * 8) + 0x1f4;
        sv2 = ((v2 & 0xff) * 8) + 0x1f4;
        sv3 = ((v3 & 0xff) * 8) + 0x1f4;
        sv4 = ((v4 & 0xff) * 8) + 0x1f4;
        sv5 = ((v5 & 0xff) * 8) + 0x1f4;
        break;
    case 11:
        b13 = (uint8_t)(self->s3_param_def[param] >> 4);
        b12 = (uint8_t)(self->s3_param_def[param + 4] >> 1);
        sv0 = (v0 & 0xff) << 4;
        sv1 = (v1 & 0xff) << 4;
        sv2 = (v2 & 0xff) << 4;
        sv3 = (v3 & 0xff) << 4;
        sv4 = (v4 & 0xff) << 4;
        sv5 = (v5 & 0xff) << 4;
        break;
    case 12:
        /* the fourth formant has no bandwidth here, so b12 stays unset */
        b13 = (uint8_t)(self->s3_param_def[param] >> 4);
        sv0 = (v0 & 0xff) << 4;
        sv1 = (v1 & 0xff) << 4;
        sv2 = (v2 & 0xff) << 4;
        sv3 = (v3 & 0xff) << 4;
        sv4 = (v4 & 0xff) << 4;
        sv5 = (v5 & 0xff) << 4;
        break;
    default:
        break;
    }

    if (param == 9) {
        if (sv0 > 0x28a || sv1 > 0x28a || sv2 > 0x28a || sv3 > 0x28a ||
            sv4 > 0x28a || sv5 > 0x28a)
            self->s3_param[0].target -= 3;
    }

    if ((uint8_t)force != 0 || cur->value == 'u') {
        int32_t take = 0;

        if (!(cur->flags & 0x20u) && (ctl->flags & 0x20u))
            take = 1;
        else if (!((ctl->flags ^ cur->flags) & 0x20u) &&
                 cur->arg < ctl->arg)
            take = 1;
        if (take) {
            sv0 = self->s3_param_def[param];
            v0 = b13;
            moved = 1;
        }
    }

    n = self->s3_1e38;
    if (n > 0 && n != 0x7f) {
        int32_t shape = (n > 0x14) ? 0x14 : n;

        if (param == 9 &&
            (!(c_cur == 'P' || c_cur == 'T' || c_cur == 'K') ||
             b13 == (uint8_t)v0))
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, n, (uint8_t)v0, (uint8_t)v0);
        else
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        shape, n, b13, (uint8_t)v0);
        pos = self->trk_wr[param] + self->s3_1e38;
        if (self->s3_1e38 == 1) {
            self->trk_wr[param] = pos;
            self->s3_param[param].len -= self->s3_1e38;
        }
    } else {
        pos = self->trk_wr[param];
    }

    if (self->s3_runlen < 8 &&
        !(Phone_Attr(px3(cur->value) | 0x180) & 1)) {
        int32_t m0 = (sv1 - sv0) / 2;
        int32_t m1 = (sv2 - sv1) / 2;
        int32_t m2 = (sv3 - sv2) / 2;
        int32_t m3 = (sv4 - sv3) / 2;
        int32_t m4 = (sv5 - sv4) / 2;

        switch (param) {
        case 9:
            v1 = (m0 + sv0) >> 2;
            v2 = (m1 + sv1) >> 2;
            v3 = (m2 + sv2) >> 2;
            v4 = (m3 + sv3) >> 2;
            v5 = (v5 & ~0xff) | (((m4 + sv4) >> 2) & 0xff);
            break;
        case 10:
            v1 = ((m0 + sv0) - 0x1f4) >> 3;
            v2 = ((m1 + sv1) - 0x1f4) >> 3;
            v3 = ((m2 + sv2) - 0x1f4) >> 3;
            v4 = ((m3 + sv3) - 0x1f4) >> 3;
            v5 = (v5 & ~0xff) | ((((m4 + sv4) - 0x1f4) >> 3) & 0xff);
            break;
        case 11:
        case 12:
            v1 = (m0 + sv0) >> 4;
            v2 = (m1 + sv1) >> 4;
            v3 = (m2 + sv2) >> 4;
            v4 = (m3 + sv3) >> 4;
            v5 = (v5 & ~0xff) | (((m4 + sv4) >> 4) & 0xff);
            break;
        default:
            break;
        }
    }

    n = self->s3_runlen - 3;
    if (n > t4)
        t4 = n;
    if (t4 < t3)
        t3 = t4 - 1;
    if (t3 < t2)
        t2 = t3 - 1;
    if (t1 > t2)
        t1 = t2 - 1;

    if (param == 9 && c_cur == 'Y' &&
        (Phone_Attr(px3(cur->prev->value) | 0x180) & 0x40))
        v0 = (v0 & ~0xff) |
             (((sv1 + (self->s3_param_def[param] - sv1) / 2) >> 2) & 0xff);

    if (moved != 0) {
        k = self->s3_param_def[param] - sv1;
        m = (k < 0) ? -k : k;
        if (m > 0xc8) {
            switch (param) {
            case 9:
                v0 = (v0 & ~0xff) | (((sv1 + k / 2) >> 2) & 0xff);
                break;
            case 10:
                v0 = (v0 & ~0xff) | ((((sv1 + k / 2) - 0x1f4) >> 3) & 0xff);
                break;
            case 11:
            case 12:
                v0 = (v0 & ~0xff) | (((sv1 + k / 2) >> 4) & 0xff);
                break;
            default:
                break;
            }
        }
    }

    Track_Decay(self, self->trk_buf[param], pos, 1, 1, (uint8_t)v0,
                (uint8_t)v0);
    if (c_cur == 'Y' &&
        (Phone_Attr(px3(cur->prev->value) | 0x180) & 0x40)) {
        k = (int32_t)cur->arg + 1;
        Track_BlendBack(self, self->trk_buf[param], pos, k, k,
                        (uint8_t)v0);
    }
    pos++;

    n = t1;
    if (n > 0 && !(Phone_Attr(px3(cur->value) | 0x180) & 1)) {
        if (n > 0x14)
            n = 0x14;
        if (n == 1)
            Track_Decay(self, self->trk_buf[param], pos, n, n, (uint8_t)v1,
                        (uint8_t)v1);
        else
            Track_Decay(self, self->trk_buf[param], pos, n, n, (uint8_t)v0,
                        (uint8_t)v1);
        pos += n;
    }

    n = t2 - t1;
    if (n > 0) {
        if (Phone_Attr(px3(cur->value) | 0x180) & 1) {
            n = t2;
            Track_RampTo(self->trk_buf[param], pos, t2, (uint8_t)v0,
                         (uint8_t)v2);
        } else {
            Track_RampTo(self->trk_buf[param], pos, n, (uint8_t)v1,
                         (uint8_t)v2);
        }
        pos += n;
        if (c_cur == 'Y' &&
            (Phone_Attr(px3(cur->prev->value) | 0x180) & 0x40)) {
            S3Param *p = &self->s3_param[param];

            self->trk_wr[param] += t1;
            if (b13 == (uint8_t)v0)
                p->shape_in = t1 + 1;
            else
                p->shape_in = (int32_t)cur->arg + t1 + 2;
            p->lead = sv1;
            p->len -= t1;
        }
    } else if (Phone_Attr(px3(cur->value) | 0x180) & 1) {
        Track_RampTo(self->trk_buf[param], pos, t2, (uint8_t)v0,
                     (uint8_t)v2);
        pos += t2;
    }

    n = t3 - t2;
    if (n > 0) {
        Track_RampTo(self->trk_buf[param], pos, n, (uint8_t)v2, (uint8_t)v3);
        pos += n;
    }
    n = t4 - t3;
    if (n > 0) {
        Track_RampTo(self->trk_buf[param], pos, n, (uint8_t)v3, (uint8_t)v4);
        pos += n;
    }
    n = self->s3_runlen - t4 - 1;
    tmp14 = n;
    if (n > 0) {
        if (n > 0x14) {
            n = 0x14;
            tmp14 = 0x14;
        }
        Track_RampTo(self->trk_buf[param], pos, n, (uint8_t)v4, (uint8_t)v5);
    }

    if (param == 12)
        return;
    param += 4;

    n = self->s3_1e38;
    if (n > 0 && n != 0x7f) {
        tmp14 = (n > 0x14) ? 0x14 : n;
        if (tmp14 < 4 && self->s3_runlen > 4 && self->s3_runlen < 8) {
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param], 3,
                        3, b12, (uint8_t)bw0);
            tmp14 = 3;
            pos = self->trk_wr[param] + 3;
        } else if (self->s3_runlen > 7) {
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        tmp14 + 3, n + 3, b12, (uint8_t)bw0);
            pos = self->trk_wr[param] + self->s3_1e38 + 3;
        } else {
            Track_Decay(self, self->trk_buf[param], self->trk_wr[param],
                        tmp14, n, b12, (uint8_t)bw0);
            pos = self->trk_wr[param] + self->s3_1e38;
        }
        self->trk_wr[param] += self->s3_1e38;
        self->s3_param[param].len -= self->s3_1e38;
    } else {
        pos = self->trk_wr[param];
    }

    m = self->s3_runlen;
    half = (m + 1) / 2;
    quarter = (m + 3) / 4;
    rest = m - quarter - half;
    n = self->s3_1e38;
    if (n > 0 && n != 0x7f && tmp14 < 4 && m > 4 && m < 8)
        half += 3 - n;
    if (m > 7) {
        if (n <= 0 || n == 0x7f) {
            Track_Decay(self, self->trk_buf[param], pos, 3, 3, b12,
                        (uint8_t)bw0);
            pos += 3;
        }
        half -= 3;
    }

    k = (half > 0x14) ? 0x14 : half;
    Track_Decay(self, self->trk_buf[param], pos, k, half, (uint8_t)bw0,
                (uint8_t)bw1);
    pos += half;
    if (quarter > 0) {
        k = (quarter > 0x14) ? 0x14 : quarter;
        Track_Decay(self, self->trk_buf[param], pos, k, quarter,
                    (uint8_t)bw1, (uint8_t)bw2);
        pos += quarter;
    }
    if (rest > 0) {
        k = (rest > 0x14) ? 0x14 : rest;
        Track_Decay(self, self->trk_buf[param], pos, k, rest, (uint8_t)bw2,
                    (uint8_t)bw2);
    }
    self->trk_wr[param] += self->s3_runlen;
    self->s3_param[param].len -= self->s3_runlen;
}

/* The stand-ins the reduction tables fall back on. */
/* @0x10039a18 */ extern const uint8_t g_red_alt1[0x37];
/* @0x10039a70 */ extern const uint8_t g_red_alt2[0x35];
/* Which of the seven table groups a vowel belongs to. */
/* @0x10039b04 */ extern const uint8_t g_red_sel[0x4a];

/* The four tables of records each group keeps, and the two headers. */
/* @0x1012c470 */ extern const tv_ref g_gt0_rec1[];
/* @0x1012c480 */ extern const tv_ref g_gt0_rec2[];
/* @0x1012c460 */ extern const tv_ref g_gt0_rec0[];
/* @0x1012c490 */ extern const tv_ref g_gt0_rec3[];
/* @0x1010b368 */ extern const tv_ref g_gt1_rec1[];
/* @0x1010b378 */ extern const tv_ref g_gt1_rec2[];
/* @0x1010b358 */ extern const tv_ref g_gt1_rec0[];
/* @0x1010b388 */ extern const tv_ref g_gt1_rec3[];
/* @0x100aecc0 */ extern const tv_ref g_gt2_rec1[];
/* @0x100aecd0 */ extern const tv_ref g_gt2_rec2[];
/* @0x100aecb0 */ extern const tv_ref g_gt2_rec0[];
/* @0x100aece0 */ extern const tv_ref g_gt2_rec3[];
/* @0x101024e4 */ extern const tv_ref g_gt3_rec1[];
/* @0x101024f4 */ extern const tv_ref g_gt3_rec2[];
/* @0x101024d4 */ extern const tv_ref g_gt3_rec0[];
/* @0x10102504 */ extern const tv_ref g_gt3_rec3[];
/* @0x100c895c */ extern const tv_ref g_gt4_rec1[];
/* @0x100c896c */ extern const tv_ref g_gt4_rec2[];
/* @0x100c894c */ extern const tv_ref g_gt4_rec0[];
/* @0x100c897c */ extern const tv_ref g_gt4_rec3[];
/* @0x100f8054 */ extern const tv_ref g_gt5_rec1[];
/* @0x100f8064 */ extern const tv_ref g_gt5_rec2[];
/* @0x100f8044 */ extern const tv_ref g_gt5_rec0[];
/* @0x100f8074 */ extern const tv_ref g_gt5_rec3[];

/* How many bits a field of this many values needs. */
static int32_t red_width(int32_t v)
{
    int32_t p = 2, n = 1, x = v / 2;

    while (x != 1) {
        p += p;
        n++;
        x /= 2;
    }
    return (v > p) ? n + 1 : n;
}

/* One of eight stand-ins, by what the phoneme is closest to. */
static uint8_t red_alt(const uint8_t *sel, int32_t limit, uint8_t c)
{
    int32_t i = px3(c) - 0x44;

    if ((uint32_t)i > (uint32_t)limit)
        return c;
    switch (sel[i]) {
    case 0: return 't';
    case 1: return 'l';
    case 2: return 'x';
    case 3: return 'L';
    case 4: return 'j';
    case 5: return 'D';
    case 6: return 'X';
    case 7: return 'J';
    default: return c;
    }
}

/*
 * Is this vowel reduced, and if so what does it become?
 *
 * A vowel between two consonants in an unstressed syllable does not hold its
 * own formants: it passes through a path the tables keep for that pair.  The
 * pair is looked up the same way the glide tables are, and what comes back
 * is a bit-packed record -- four formants, the frames each is reached at and
 * the bandwidths -- which is unpacked here and handed to the track writers.
 * Returns 1 when the vowel was reduced, 0 when it keeps its own targets.
 */
/* @0x10038890 */
uint8_t TV_THISCALL Stage3_Reduce(Engine *self, int32_t which)
{
    StageCtx *st = &self->stage_ctx[3];
    Node *ctl = st->ctl;
    Node *cur = st->cur;
    Node *scan = st->scan;
    Node *nx;
    const int8_t *p24;
    const uint8_t *data, *info, *base2, *rec;
    uint8_t c_cur = cur->value;
    uint8_t c_scan = scan->value;
    uint8_t c_ctl = ctl->value;
    uint8_t back = cur->prev->value;
    uint8_t s2 = scan->next->value;
    uint8_t first_cur, first_scan, isv = 0, a, d;
    int32_t kind = 3, lead = 0, limit = 0, v24;
    int32_t rank, row, bit, pass, stride, off = 0;
    int32_t i, j, k, v, w;
    const uint8_t *rc[4];
    int32_t wid[4], fld[4];
    int32_t raw[9];
    int32_t runlen1;

    if (Phone_IsVowel(c_cur))
        isv = 1;

    if (Phone_Attr(px3(c_cur) | 0x100) & 2) {
        if (c_cur == 'A' || c_cur == 'E' || c_cur == 'I' || c_cur == 'a' ||
            c_cur == 'e' || c_cur == 'i' || c_cur == 'y' || c_cur == '|')
            c_cur = 'Y';
        else if (c_cur == 'O' || c_cur == 'U' || c_cur == 'b' ||
                 c_cur == 'u' || c_cur == 'f')
            c_cur = 'W';
        else if (c_cur == '3' || c_cur == '4' || c_cur == 'k' ||
                 c_cur == 'c' || c_cur == 'g' || c_cur == 'r' ||
                 c_cur == '5')
            c_cur = 'R';
        else
            c_cur = ' ';
    } else if (c_cur == 'n') {
        c_cur = 'N';
    }

    if (Phone_Attr(px3(c_scan) | 0x100) & 2) {
        if (c_ctl == '3' || c_ctl == 'k' || c_ctl == 'r' || c_ctl == 'g' ||
            c_ctl == '4' || c_ctl == 'c' || c_ctl == '5')
            c_scan = 'R';
        else if (c_ctl == 'b' || c_ctl == 'f' || c_ctl == 'O' ||
                 c_ctl == 'U')
            c_scan = 'W';
        else if (c_ctl == '@' || c_ctl == 'v' || c_ctl == 'o' ||
                 c_ctl == 'w')
            c_scan = ' ';
        else
            c_scan = 'Y';
    }

    a = (uint8_t)(Phone_Attr(px3(c_ctl) | 0x180) & 0x40);
    k = 0;
    if (a != 0 && c_ctl != '5' && c_ctl != 'E') {
        nx = ctl->next;
        d = nx->value;
        if (d == '&' || d == '%') {
            d = nx->next->value;
            if (d != 'R' && d != 'L' && d != 'W' && d != 'Y' && d != 'd' &&
                d != 'H' && d != 't' &&
                (c_ctl != 'I' || (ctl->flags & 0x20u))) {
                c_scan = ' ';
                k = 1;
            }
        }
    }
    if (!k && c_scan == 'n')
        c_scan = 'N';

    nx = ctl->next;
    d = nx->value;
    if ((d == '&' || d == '%') && nx->next->value == 'N' &&
        c_ctl != '@' && c_ctl != '|' && c_ctl != '5' &&
        (ctl->flags & 0x20u))
        c_scan = ' ';

    if (c_ctl == 'E' && c_cur == 'M' && c_scan == ' ' &&
        nx->next->value == 'M')
        c_scan = 'M';

    if ((d == '&' || d == '%') && nx->next->b18 == 1)
        c_scan = nx->next->value;

    {
        uint8_t prev_ch = ctl->prev->value;

        if (prev_ch == '&' || prev_ch == '%') {
            if ((Phone_Attr(px3(c_cur)) & 0x40) && c_cur != 'Z' &&
                c_cur != 'S' && (ctl->flags & 0x20u)) {
                int32_t blank = 0;

                if (a != 0 && ctl->value != 'I') {
                    blank = 1;
                } else if ((Phone_Attr(px3(d)) & 0x10) &&
                           (ctl->flags & 0x20u) && ctl->value != 'e' &&
                           c_cur != 'X' &&
                           !(c_cur == 's' && c_ctl == '3') &&
                           !(c_cur == 'V' && c_ctl == 'a')) {
                    blank = 1;
                }
                if (blank)
                    c_cur = ' ';
            }
        }
        if (c_ctl == 'v' && d == 'N' && prev_ch == '&')
            c_cur = ' ';
        if ((prev_ch == '&' || prev_ch == '%') && ctl->prev->b14 == 1)
            c_cur = ' ';
    }
    if ((d == '&' || d == '%') && nx->b14 == 1)
        c_scan = ' ';

    if ((Phone_Attr(px3(c_ctl) | 0x80) & 0x10) && !(ctl->flags & 0x20u) &&
        d == 'q' && self->s3_1fec == ' ')
        c_scan = 'T';

    if (c_ctl == 'E' && c_cur == 'Z' && c_scan == 'Y' && d == 'Y' &&
        nx->next->value == '@') {
        which = Vowel_Index('i');
        c_ctl = 'i';
    }

    v = (int32_t)ctl->arg;
    self->s3_runlen = v;
    k = self->s3_1e38;
    if (k != 0 && k != 0x7f)
        self->s3_runlen = v - k;
    if (self->s3_runlen < 1)
        self->s3_runlen = 1;
    runlen1 = self->s3_runlen - 1;

    row = (int32_t)g_phone_bit[px3(c_cur)];
    if (row == -1)
        return 0;
    p24 = &g_phone_bit[px3(c_scan)];
    bit = (int32_t)*p24;
    if (bit == -1)
        return 0;
    v24 = (int32_t)(uintptr_t)p24;
    rank = Bits_Rank(bit, row, which);
    first_cur = c_cur;
    first_scan = c_scan;

    for (pass = 0; pass < 2 && rank == -1; pass++) {
        c_scan = red_alt(g_red_alt1, 0x36, c_scan);
        if (c_scan != first_scan) {
            bit = (int32_t)g_phone_bit[px3(c_scan)];
            if (bit != -1)
                rank = Bits_Rank(bit, row, which);
        }
        if (rank == -1 &&
            (c_scan == 'j' || c_scan == 'L' || c_scan == 'l')) {
            c_scan = glide_liquid(first_scan);
            bit = (int32_t)g_phone_bit[px3(c_scan)];
            if (bit != -1)
                rank = Bits_Rank(bit, row, which);
        }
        if (rank == -1 && first_cur == c_cur) {
            c_scan = first_scan;
            bit = (int32_t)*p24;
            c_cur = red_alt(g_red_alt2, 0x34, c_cur);
            if (first_cur != c_cur) {
                row = (int32_t)g_phone_bit[px3(c_cur)];
                if (row != -1)
                    rank = Bits_Rank(bit, row, which);
            }
        }
        if (rank == -1 &&
            (first_cur == 'j' || first_cur == 'l' || first_cur == 'L')) {
            c_cur = glide_liquid(first_cur);
            row = (int32_t)g_phone_bit[px3(c_cur)];
            if (row != -1)
                rank = Bits_Rank(bit, row, which);
        }
    }
    if (rank == -1)
        return 0;

    if (c_ctl != '@' && c_ctl != '|')
        kind = 6;

    /* two of the groups leave these unset in the original; the corpus
     * never takes those arms */
    data = (const uint8_t *)p24;
    info = (const uint8_t *)p24;
    limit = v24;
    rc[0] = (const uint8_t *)p24;
    rc[1] = (const uint8_t *)p24;
    rc[2] = (const uint8_t *)p24;
    rc[3] = (const uint8_t *)p24;
    i = px3(c_ctl) - 0x33;
    switch (((uint32_t)i <= 0x49) ? g_red_sel[i] : 22) {
    case 0: case 2: case 14: case 18:
        data = TV_REF(uint8_t, g_gt4_data[which]); info = TV_REF(uint8_t, g_gt4_info[which]);
        limit = g_gt4_max[which];
        rc[0] = TV_REF(uint8_t, g_gt4_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt4_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt4_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt4_rec3[which]);
        break;
    case 1: case 11: case 16:
        data = TV_REF(uint8_t, g_gt5_data[which]); info = TV_REF(uint8_t, g_gt5_info[which]);
        limit = g_gt5_max[which];
        rc[0] = TV_REF(uint8_t, g_gt5_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt5_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt5_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt5_rec3[which]);
        break;
    case 3: case 21:
        data = TV_REF(uint8_t, g_gt6_data[which]);
        limit = g_gt6_max[which];
        break;
    case 4: case 5: case 6: case 20:
        data = TV_REF(uint8_t, g_gt1_data[which]); info = TV_REF(uint8_t, g_gt1_info[which]);
        limit = g_gt1_max[which];
        rc[0] = TV_REF(uint8_t, g_gt1_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt1_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt1_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt1_rec3[which]);
        break;
    case 7: case 8: case 10: case 13:
        data = TV_REF(uint8_t, g_gt0_data[which]); info = TV_REF(uint8_t, g_gt0_info[which]);
        limit = g_gt0_max[which];
        rc[0] = TV_REF(uint8_t, g_gt0_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt0_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt0_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt0_rec3[which]);
        break;
    case 9: case 12: case 15:
        data = TV_REF(uint8_t, g_gt2_data[which]); info = TV_REF(uint8_t, g_gt2_info[which]);
        limit = g_gt2_max[which];
        rc[0] = TV_REF(uint8_t, g_gt2_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt2_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt2_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt2_rec3[which]);
        break;
    case 17: case 19:
        data = TV_REF(uint8_t, g_gt3_data[which]); info = TV_REF(uint8_t, g_gt3_info[which]);
        limit = g_gt3_max[which];
        rc[0] = TV_REF(uint8_t, g_gt3_rec0[which]); rc[1] = TV_REF(uint8_t, g_gt3_rec1[which]);
        rc[2] = TV_REF(uint8_t, g_gt3_rec2[which]); rc[3] = TV_REF(uint8_t, g_gt3_rec3[which]);
        break;
    default:
        break;
    }

    if (c_ctl == '@' || c_ctl == '|') {
        lead = 6;
    } else {
        lead = (int32_t)info[0];
        wid[0] = red_width(((int32_t)info[1] << 8) + (int32_t)info[2]);
        wid[1] = red_width(((int32_t)info[3] << 8) + (int32_t)info[4]);
        wid[2] = red_width(((int32_t)info[5] << 8) + (int32_t)info[6]);
        wid[3] = red_width((int32_t)info[7]);
        v24 = (int32_t)info[9];
    }

    stride = kind + lead + 9;
    off = rank * stride;
    k = (int32_t)data[kind + off + lead + 8];
    k = Stage3_FindRow(self, k, which, back, s2, v24);
    if (k != -1) {
        rank = k;
        off = k * stride;
    }

    if (rank == -1 || rank >= limit)
        return 0;

    base2 = data + lead + off;

    if (c_ctl == '@' || c_ctl == '|') {
        const uint8_t *p0 = data + off;

        for (i = 0; i < 4; i++) {
            int32_t settle = (int32_t)p0[i];
            int32_t hi = (int32_t)data[off + i / 2 + 4];

            if (i == 0 || i == 2)
                hi = (hi & 0xf0) >> 4;
            else
                hi = hi & 0xf;
            hi += 3;
            hi = (int32_t)(((uint32_t)(hi * runlen1) * 5u + 0x32u) / 100u);
            v = (int32_t)base2[i];
            k = (int32_t)base2[i + 4];
            w = (int32_t)base2[i + 8];
            j = 0x64;
            if (i < 3)
                j = self->s3_param_def[13 + i] >> 1;
            Stage3_LayGlide(self, settle, hi, v, k, w, i + 9, isv);
            Stage3_SetGlide(self, k, w, v, j, i + 9, isv);
        }
        return 1;
    }

    /* unpack the four packed fields that head the record */
    {
        int32_t avail = 8;
        int32_t byte_i = 0;
        int32_t cur_b = (int32_t)data[off];

        for (i = 0; i < 4; i++) {
            int32_t val;

            w = wid[i];
            if (avail == 8) {
                val = cur_b;
            } else {
                k = 8 - avail;
                val = ((cur_b << k) & 0xff) >> k;
            }
            if (w > avail) {
                k = w - avail;
                byte_i++;
                val <<= k;
                avail = 8 - k;
                cur_b = (int32_t)data[off + byte_i];
                val += cur_b >> avail;
            } else if (w == avail) {
                if (i != 3) {
                    byte_i++;
                    cur_b = (int32_t)data[off + byte_i];
                }
                avail = 8;
            } else {
                avail -= w;
                val >>= avail;
            }
            fld[i] = val;
        }
    }

    /* and the nine that follow it */
    {
        int32_t avail = 8;
        int32_t byte_i = 0;
        int32_t cur_b = (int32_t)base2[8];

        for (i = 0; i < 9; i++) {
            int32_t val;

            w = (i <= 2) ? 6 : 5;
            if (avail == 8) {
                val = cur_b;
            } else {
                k = 8 - avail;
                val = ((cur_b << k) & 0xff) >> k;
            }
            if (w > avail) {
                k = w - avail;
                byte_i++;
                val <<= k;
                avail = 8 - k;
                cur_b = (int32_t)base2[byte_i + 8];
                val += cur_b >> avail;
            } else if (w == avail) {
                avail = 8;
            } else {
                avail -= w;
                val >>= avail;
            }
            raw[i] = val;
        }
    }

    {
        int32_t amp[3], bw_a[3], bw_b[3];

        amp[0] = (raw[0] & 0x3f) << 3;
        amp[1] = (raw[1] & 0x3f) << 3;
        amp[2] = (raw[2] & 0x3f) << 3;
        bw_a[0] = (raw[3] * 12) & 0x1ff;
        bw_a[1] = (raw[4] * 12) & 0x1ff;
        bw_a[2] = (raw[5] * 12) & 0x1ff;
        bw_b[0] = (raw[6] * 12) & 0x1ff;
        bw_b[1] = (raw[7] * 12) & 0x1ff;
        bw_b[2] = (raw[8] * 12) & 0x1ff;

        {
            const uint8_t *tab = 0;
            int32_t bw0 = 0, bw1 = 0, bw2 = 0;
            int32_t t1, t2, t3, t4, v0, v5;

            for (i = 0; i < 4; i++) {
                S3Param *p = &self->s3_param[9 + i];

                switch (i) {
                case 0:
                    tab = rc[0]; v = 6 * fld[0];
                    bw0 = amp[0]; bw1 = bw_a[0]; bw2 = bw_b[0];
                    break;
                case 1:
                    tab = rc[1]; v = 6 * fld[1];
                    bw0 = amp[1]; bw1 = bw_a[1]; bw2 = bw_b[1];
                    break;
                case 2:
                    tab = rc[2]; v = 6 * fld[2];
                    bw0 = amp[2]; bw1 = bw_a[2]; bw2 = bw_b[2];
                    break;
                default:
                    tab = rc[3]; v = 6 * fld[3];
                    break;
                }
                rec = tab + v;
                t1 = (int32_t)(((uint32_t)((((int32_t)rec[4] & 0xf0) >> 4)
                                           + 0xa) * (uint32_t)runlen1
                                + 0x32u) / 100u);
                t2 = (int32_t)(((uint32_t)((((int32_t)rec[4] & 0xf) * 2)
                                           + 0x14) * (uint32_t)runlen1
                                + 0x32u) / 100u);
                t3 = (int32_t)(((uint32_t)((((int32_t)rec[5] & 0xf0) >> 3)
                                           + 0x32) * (uint32_t)runlen1
                                + 0x32u) / 100u);
                t4 = (int32_t)(((uint32_t)((((int32_t)rec[5] & 0xf) * 2)
                                           + 0x41) * (uint32_t)runlen1
                                + 0x32u) / 100u);
                v0 = (int32_t)base2[i];
                v5 = (int32_t)base2[i + 4];
                if (i < 3) {
                    if ((uint32_t)bw0 > 0xc8u)
                        bw0 = 0xc8;
                    if ((uint32_t)bw1 > 0xc8u)
                        bw1 = 0xc8;
                    if ((uint32_t)bw2 > 0xc8u)
                        bw2 = 0xc8;
                    bw0 = (int32_t)((uint32_t)bw0 >> 1);
                    bw1 = (int32_t)((uint32_t)bw1 >> 1);
                    bw2 = (int32_t)((uint32_t)bw2 >> 1);
                }
                Stage3_LayPath(self, (int32_t)rec[0], (int32_t)rec[1],
                               (int32_t)rec[2], (int32_t)rec[3], t1, t2, t3,
                               t4, bw1, v5, bw2, v0, bw0, i + 9, isv);
                Stage3_SetGlide(self, v5, bw2, v0, bw0, i + 9, isv);

                k = p->len;
                if (k <= 4) {
                    Track_RampTo(self->trk_buf[9 + i],
                                 self->trk_wr[9 + i] + 1, k - 1,
                                 (uint8_t)v0, (uint8_t)v5);
                } else if (k <= 7) {
                    int32_t mid = ((int32_t)rec[3] + (int32_t)rec[2] +
                                   (int32_t)rec[1] + (int32_t)rec[0]) / 4;
                    int32_t half = (k - 1) / 2;

                    Track_RampTo(self->trk_buf[9 + i],
                                 self->trk_wr[9 + i] + 1, half, (uint8_t)v0,
                                 (uint8_t)mid);
                    k = p->len;
                    half = (k - 1) / 2;
                    Track_RampTo(self->trk_buf[9 + i],
                                 self->trk_wr[9 + i] + half + 1,
                                 k + (1 - k) / 2 - 1, (uint8_t)mid,
                                 (uint8_t)v5);
                }
            }
        }
    }
    return 1;
}
