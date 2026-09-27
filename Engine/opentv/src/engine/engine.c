/*
 * Engine object lifecycle and parameter setters.
 */
#include "engine.h"

/* Default raw values of the 22 synthesis parameters for a new engine. */
/* @0x100f8530 */
extern const uint8_t g_default_params[22];

/* @0x10030fc0 */
Engine *TV_THISCALL Engine_Construct(Engine *self)
{
    memset(self, 0, offsetof(Engine, synth_hold));
    self->sample_rate = 8000;
    self->w_2130 = 0;
    self->w_2132 = 0;
    self->sapi = NULL;
    self->item_notify = 0;
    self->item_done = 1;
    self->cur_voice = 0;
    self->cur_bac = 0;
    self->fmt_2104 = 1;
    self->cur_pitch = 85;
    self->cur_speed = 150;
    self->cur_volume = 0xffff;
    return self;
}

/* Default voice parameters, then a full reset. */
/* @0x1002c450 */
int32_t TV_THISCALL Engine_Init(Engine *self)
{
    int i;
    self->mode_P = 1;
    self->rate_index = 13;
    self->speed_wpm = 150;
    self->pitch = 85;
    self->flags_N = 0x17c0;
    self->flags_A = 0x41;
    self->mode_I = 0;
    self->volume_atten = 0;
    self->rate_class = 0;
    self->voice = 0;
    for (i = 0; i < 22; i++)
        self->cfg_bytes[i] = g_default_params[i];
    self->stop_mark = 0;
    return Engine_Reset(self);
}

/* Clear all pipeline state.  Returns reset_value if a reset was requested
 * with one pending (stop_mark), else -1. */
/* @0x1002c4d0 */
int32_t TV_THISCALL Engine_Reset(Engine *self)
{
    int32_t ret = -1;
    if (self->stop_mark)
        ret = self->reset_value;
    self->st_2110 = 0;
    self->st_input_empty = 1;
    self->st_idle = 1;
    self->st_2113 = 0;
    self->st_2114 = 0;
    self->st_2115 = 0;
    self->st_2116 = 0;
    self->st_2117 = 0;
    self->skip_text = 0;
    self->reset_pending = 0;
    self->stop_mark = 0;
    self->out_state = 2;
    Preformat_Reset(self);
    Engine_ResetNodes(self);
    Synth_ResetTracks(self);
    Synth_InitFilters(self);
    Engine_ResetRings(self);
    Output_Reset(self, (int16_t)self->sample_rate);
    Stage4_Reset(self);
    Prosody_Reset(self);
    Stage3_Reset(self);
    Stage2_Reset(self);
    Stage1_Reset(self);
    Stage0_Reset(self);
    return ret;
}

/* @0x1002c810 */
void TV_THISCALL Engine_SetPitch(Engine *self, int32_t pitch)
{
    int i;
    self->pitch = pitch;
    for (i = 0; i < 5; i++)
        self->stage_ctx[i].pitch = pitch;
}

/* @0x1002c840 */
void TV_THISCALL Engine_SetSpeed(Engine *self, int32_t wpm)
{
    int32_t idx = (int32_t)((uint32_t)(wpm - 46) >> 3);
    int i;

    /* OpenTV: the original lets this index off the end of a 26-row
     * table, and wraps unsigned below 46 into a wild one.  With the
     * extension on it is held inside the rows that exist; with it off
     * it is left alone, so the corpus still checks both faults. */
    if (tv_ext_rate) {
        if (wpm < 46)
            idx = 0;
        else if (idx > TV_RATE_ROW_MAX)
            idx = TV_RATE_ROW_MAX;
    }
    self->speed_wpm = wpm;
    self->rate_index = idx;
    for (i = 0; i < 5; i++)
        self->stage_ctx[i].rate_index = idx;
}

/* SAPI volume (0..0xffff linear) to attenuation in dB, at most 15; below
 * 0x50 the output is muted instead. */
/* @0x1002c870 */
void TV_THISCALL Engine_SetVolume(Engine *self, uint32_t vol)
{
    int32_t att;
    int i;
    if (vol < 0x50) {
        self->mute = 1;
        return;
    }
    self->mute = 0;
    att = Volume_ToAtten(vol);
    self->volume_atten = att;
    for (i = 0; i < 5; i++)
        self->stage_ctx[i].volume_atten = att;
}

/* @0x1002c8f0 */
void TV_THISCALL Engine_SetVoice(Engine *self, uint32_t voice)
{
    int i;
    if (voice >= 10)
        return;
    self->voice = (int32_t)voice;
    for (i = 0; i < 5; i++)
        self->stage_ctx[i].voice = (int32_t)voice;
}

/* Clear the rings and the input-side state. */
/* @0x10027ea0 */
void TV_THISCALL Engine_ResetRings(Engine *self)
{
    self->in_wr = 0;
    self->in_rd = 0;
    self->pre_wr = 0;
    self->pre_rd = 0;
    self->mid_wr = 0;
    self->mid_rd = 0;
    self->e_9180 = -1;
    self->in_flags_N = 0x17c0;
    self->in_flags_A = 0x41;
}

/* @0x10002a20 */
void TV_THISCALL Prosody_Reset(Engine *self)
{
    self->e_206c = 85;
    self->e_2074 = 0;
    self->e_2070 = 0;
}

/* One scheduling step of the pipeline.  Downstream work always goes first:
 * audio is generated (Synth_Step, in step with Stage 4) until about 12000
 * bytes are waiting; otherwise the furthest stage with pending work runs,
 * provided enough nodes are free.  New input is read only when every later
 * stage is idle.
 *
 * Returns 2 when PCM is ready in out_buf, 0 otherwise; when the input is
 * exhausted and everything is idle it returns 1, or 3 if PCM remains. */
/* @0x1002c5a0 */
int32_t TV_THISCALL Engine_Step(Engine *self)
{
    uint8_t produced = 0;

    if (self->reset_pending)
        Engine_Reset(self);
    if (self->out_state != 2)
        self->out_state = Stage4_Run(self);

    if (self->st_2117) {
        if (Tracks_Op(self, 1, self->trk_38))
            self->st_2117 = 0;
    }
    if (!self->st_2117 && self->st_2116) {
        switch (Stage3_Run(self)) {
        case 0:
            self->st_2116 = 0;
            break;
        case 1:
            self->st_2117 = 1;
            break;
        }
        for (;;) {
            if (self->synth_busy || self->synth_hold)
                break;
            if (!Synth_Step(self))
                break;
            if (!self->synth_19ad)
                break;
            if (self->trk_34 != 0) {
                produced = 1;
                self->out_state = Stage4_Run(self);
                if (self->out_count > 12000)
                    break;
            } else if (self->out_count > 12000) {
                produced = 1;
                break;
            }
        }
        self->out_state = Stage4_Run(self);
    } else if (self->st_2115) {
        if (self->free_nodes > 0x69) {
            if (Stage2_Run(self))
                self->st_2116 = 1;
            else
                self->st_2115 = 0;
        }
    } else if (self->st_2114) {
        if (self->free_nodes > 0x25) {
            if (Stage1_Run(self))
                self->st_2115 = 1;
            else
                self->st_2114 = 0;
        }
    } else if (self->st_2113) {
        if (self->free_nodes > 0x25) {
            if (Stage0_Run(self))
                self->st_2114 = 1;
            else
                self->st_2113 = 0;
        }
    } else if (self->st_idle) {
        return (self->out_count != 0 ? 2 : 0) + 1;
    } else if (self->free_nodes > 0xf) {
        if (Engine_InputStage(self))
            self->st_2113 = 1;
        else
            self->st_idle = 1;
    }
    return produced ? 2 : 0;
}
