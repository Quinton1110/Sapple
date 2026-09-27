/*
 * Control commands.
 *
 * Type-0 nodes carry the commands the preformatter parsed out of the text
 * (ESC[..X).  Every stage executes them as its cursor passes, so a change
 * takes effect at the right point of that stage's work; most commands only
 * act in the one stage that owns the setting, and the values are kept
 * per stage in StageCtx.
 */
#include "engine.h"
#include "crt.h"

/* Per-voice defaults, indexed by voice number. */
/* @0x100b5350 */
extern const uint32_t g_voice_pitch[10];
/* @0x100b5378 */
extern const uint32_t g_voice_rate_index[10];
/* @0x100b53a0 */
extern const uint32_t g_voice_speed[10];
/* TTSMODEINFO block per voice, handed to the SAPI layer on a voice change. */
/* @0x1013af08 */
extern const uint8_t g_mode_info[];

#define MODE_INFO_SIZE 0xaf0

/* Execute the control node the running stage has reached.  Returns 0 only
 * when stage 2 meets a node it must stop at. */
/* @0x10028340 */
uint8_t TV_THISCALL Engine_RunControl(Engine *self)
{
    StageCtx *st = self->stage;
    Node *n = st->ctl;
    int32_t stage = (int32_t)(st - self->stage_ctx);
    SapiCentral *sapi = self->sapi;
    uint32_t flags;
    int32_t arg, v;
    uint8_t b15;

    if (stage == 2 && NODE_TYPE(n) == 5)
        return 0;
    flags = n->flags;
    if (flags & NODE_TYPE_MASK)
        return 1;

    arg = (int32_t)n->arg;
    if (n->value != 'i')
        arg &= 0xff;
    b15 = n->b15;

    switch (n->value) {
    case 'A':
        st->p_38 = ((int32_t)b15 << 8) | arg;
        break;
    case 'C':
        if (stage == 0)
            Engine_NodeAlloc(self, n, 0, 3, ']');
        break;
    case 'I':
        st->p_1c = arg;
        break;
    case 'N':
        st->p_34 = ((int32_t)b15 << 8) | arg;
        break;
    case 'P':
        st->p_20 = ((int32_t)b15 << 8) + arg;
        break;
    case 'V':
        st->voice = arg;
        self->stage->pitch = (int32_t)g_voice_pitch[self->stage->voice];
        self->stage->rate_index = (int32_t)g_voice_rate_index[self->stage->voice];
        if (stage != 3)
            break;
        self->cur_voice = (int16_t)arg;
        self->cur_speed = g_voice_speed[(int16_t)arg];
        self->cur_pitch = (uint32_t)self->stage->pitch;
        if (sapi == NULL)
            break;
        sapi->voice = (int16_t)arg;
        memcpy(sapi->mode_info, g_mode_info + (int32_t)self->cur_voice * MODE_INFO_SIZE,
               MODE_INFO_SIZE);
        sapi->pitch = (int16_t)self->cur_pitch;
        sapi->speed = (int32_t)self->cur_speed;
        break;
    case 'a':
        st->volume_atten = arg;
        if (stage != 3)
            break;
        if (arg == 0x10) {
            self->cur_volume = 0;
            self->stage->volume_atten = 100;
        } else {
            self->cur_volume = Volume_FromAtten(arg);
            self->mute = 0;
        }
        if (sapi == NULL)
            break;
        sapi->volume = (int32_t)self->cur_volume;
        Sapi_Post(sapi, 0x46a, 3, 0);
        break;
    case 'c':
        if (stage != 2 || sapi == NULL)
            break;
        self->cur_bac = arg;
        sapi->ctx = arg;
        break;
    case 'f':
        if (st->p_30 == 0)
            self->e_1ae4 = st->rate_index;
        st->p_30 = arg;
        self->stage->rate_index = b15;
        if (self->stage->p_30 != 0)
            break;
        self->stage->rate_index = self->e_1ae4;
        break;
    case 'g':
    case 's':
    case 't':
        if (stage != 2)
            break;
        n->flags = (flags & ~2u) | 5;
        break;
    case 'i':
        if (stage != 3)
            break;
        if (sapi != NULL) {
            if (arg == 0 && n->notify != 0)
                Sapi_Post(sapi, 0x4c9, 0, n->notify);
            if (n->notify != 0) {
                uint32_t *rec = (uint32_t *)tv_new(12);
                if (rec != NULL) {
                    rec[0] = 0;
                    rec[1] = n->notify;
                    rec[2] = (uint32_t)arg;
                    if (arg == 0)
                        n->notify = 0;
                    Sapi_Lock(sapi);
                    Sapi_QueuePush(sapi, &rec, sizeof rec);
                    Sapi_Unlock(sapi);
                    Sapi_Post(sapi, 0x4c8, 0, 0);
                }
            }
        }
        if (n->flags & 0x20) {
            self->reset_pending = 1;
            self->reset_value = arg;
        }
        break;
    case 'l':
        /* the preformatter has already clamped the index below 22 */
        self->s3_param_raw[arg] = b15;
        break;
    case 'p':
        arg += arg;
        st->pitch = arg;
        if (stage != 2)
            break;
        self->cur_pitch = (uint32_t)self->stage->pitch;
        if (sapi == NULL)
            break;
        sapi->pitch = (int16_t)self->cur_pitch;
        Sapi_Post(sapi, 0x46a, 1, 0);
        break;
    case 'r':
    case 'v':
        st->rate_index = arg;
        if (stage != 2)
            break;
        v = arg * 8 + 50;
        self->cur_speed = (uint32_t)v;
        v = (int32_t)(((uint32_t)v / 10) * 10);
        self->cur_speed = (uint32_t)v;
        if (sapi == NULL)
            break;
        sapi->speed = v;
        Sapi_Post(sapi, 0x46a, 2, 0);
        break;
    case 'x':
        if (stage == 4) {
            self->reset_value = 0;
            self->reset_pending = 1;
        } else if (stage == 0) {
            Engine_NodeAlloc(self, n, 1, 0, 'C');
        }
        break;
    default:
        break;
    }
    return 1;
}
