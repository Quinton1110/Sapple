/*
 * Per-stage reset routines.  Each also fixes which node types its stage
 * processes (StageCtx.type_mask, one bit per type from g_node_type_bits).
 *
 * These will move to their stage's module once the stages are decompiled.
 */
#include "engine.h"

/* Stage 0 rule program. */
/* @0x10120a10 */
extern const uint8_t g_stage0_rules[];

/* Initial raw value of each synthesis parameter track. */
/* @0x100f8438 */
extern const uint8_t g_param_init[22];

/* @0x10031510 */
void TV_THISCALL Stage0_Reset(Engine *self)
{
    self->s0_sp = self->s0_stack;
    self->s0_1b3d = 0;
    self->s0_1c1c = 0;
    self->stage_ctx[0].type_mask = 0x17; /* types 0,1,2,3 */
    self->s0_ip = g_stage0_rules;
    self->s0_need_test = 1;
    self->s0_pending_ptr = &self->s0_pending;
}

/* @0x10060a20 */
void TV_THISCALL Stage1_Reset(Engine *self)
{
    self->stage_ctx[1].type_mask = 0x06; /* types 1,2 */
    self->s1_next_start = NULL;
    self->s1_letters = NULL;
    self->s1_next_end = NULL;
    self->s1_word_start = NULL;
    self->s1_word_end = NULL;
    self->s1_1c20 = 0;
    self->s1_1c38 = 0;
    self->s1_1c50 = 0;
}

/* @0x1002b3b0 */
void TV_THISCALL Stage2_Reset(Engine *self)
{
    self->stage_ctx[2].type_mask = 0x1c; /* types 0,3,4 */
    self->s2_1d6c = 3;
    self->s2_1d80 = -1;
    self->s2_1d7c = -1;
    self->s2_1d5c = 180;
    self->s2_1d78 = -1;
    self->s2_1d84 = 0;
    self->s2_1d70 = 0;
    self->s2_1d44 = 0;
    self->s2_1d55 = 0;
    self->s2_1d54 = 0;
    self->s2_1d88 = 0;
    self->s2_1dd4 = 0;
    self->s2_1e1c = 0;
    self->s2_1d68 = 0;
    self->s2_1d4c = 0;
    self->s2_1d48 = 0;
}

/* @0x1002d9e0 */
void TV_THISCALL Stage3_Reset(Engine *self)
{
    int i;
    self->s3_2034 = 0;
    self->s3_2035 = 0;
    self->s3_1fe8 = 4;
    self->s3_1fb0 = 0;
    self->s3_1fbc = 0;
    self->s3_1fdd = 0;
    self->s3_1fae = 1;
    self->s3_1fb4 = 1;
    self->s3_1fe4 = 3;
    self->s3_1fe0 = 2;
    self->stage_ctx[3].type_mask = 0x28; /* types 4,5 */
    self->s3_1fb8 = 0;
    for (i = 0; i < 22; i++)
        self->s3_param_raw[i] = self->cfg_bytes[i];
    Stage3_ResetParams(self);
}

/* @0x1002dee0 */
void TV_THISCALL Stage3_ResetParams(Engine *self)
{
    int i;
    self->s3_1e30 = -13120;
    self->s3_1e2c = -13120;
    self->s3_1e34 = 0;
    self->s3_1e24 = 0;
    self->s3_1e28 = 0;
    self->s3_1e40 = 0;
    self->s3_1e3c = 0;
    self->s3_1e48 = 0;
    self->s3_1e44 = 0;
    self->s3_1e50 = 0;
    self->s3_1e38 = 0;
    self->s3_1e4c = 0;
    self->s3_1e5c[2] = 0;
    self->s3_1e5c[1] = 0;
    self->s3_1e5c[0] = 0;
    self->s3_1e58 = 0;
    self->s3_1e54 = 0;
    for (i = 0; i < 22; i++) {
        self->trk_rd[i] = self->trk_0c - self->s3_1fe0;
        self->trk_wr[i] = self->trk_0c;
        self->s3_param_def[i] = Synth_ScaleParam(i, g_param_init[i]);
    }
    self->s3_1fed = 0x20;
    self->trk_10 = self->trk_0c;
}

/* @0x1002bc00 */
void TV_THISCALL Stage4_Reset(Engine *self)
{
    self->stage_ctx[4].type_mask = 0x3f; /* all live types */
}

/* Stage 4 runs in step with the audio: it executes control commands and
 * releases one type-4 node per synthesized frame (trk_34 counts frames not
 * yet matched).  Returns 0 when it must wait for more audio, 1 when a
 * control command blocked, 2 when it reached the end of its window. */
/* @0x1002bc10 */
int32_t TV_THISCALL Stage4_Run(Engine *self)
{
    StageCtx *st = &self->stage_ctx[4];
    int32_t ret = 2;

    if (!Engine_StageBegin(self, st)) {
        Engine_StageEnd(self);
        return 2;
    }
    while (st->ctl != NULL && st->last != st->ctl) {
        uint32_t type = NODE_TYPE(st->ctl);
        if (type == NODE_CONTROL) {
            if (!Engine_RunControl(self)) {
                ret = 1;
                break;
            }
        } else if (type == 4) {
            if (self->trk_34 == 0) {
                ret = 0;
                break;
            }
            self->trk_34--;
        }
        st->ctl = Engine_StageNext(self, st->ctl);
    }
    st->scan = st->ctl;
    st->cur = st->ctl;
    Engine_StageEnd(self);
    return ret;
}
