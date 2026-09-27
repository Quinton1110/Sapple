/*
 * PCM output stage state.
 */
#include "engine.h"

/* @0x100268d0 */
void TV_THISCALL Output_Reset(Engine *self, int16_t sample_rate)
{
    int i;
    self->o_2080 = 0;
    self->o_207e = 0;
    self->o_207c = 0;
    self->o_207a = 0;
    self->o_20dc = 0;
    self->o_2076 = 0;
    self->o_20e4 = 0;
    self->o_208a = 0;
    self->o_20e0 = 0;
    self->o_2086 = 0;
    self->o_2082 = 0;
    self->o_20ac = 0;
    self->o_20aa = 0;
    self->o_208e = 0;
    self->o_208c = 0;
    for (i = 0; i < 22; i++)
        self->o_20ae[i] = 0;
    for (i = 0; i < 11; i++)
        self->o_2092[i] = 0;
    self->o_20a8 = 0;
    self->o_2088 = 0xaaaa;
    self->o_rate_div100 = (int16_t)(sample_rate / 100);
    self->o_20e8 = 0;
}
