/*
 * The few places where the engine reaches back into the layer that owns it:
 * the audio queue (which carries bookmark records as well as PCM) and the
 * window notifications.  Outside the hook build the notifications go
 * nowhere -- there is no window -- and the queue is the library's, in
 * src/port/tvtts.c, which turns a bookmark record into a callback.
 */
#include "engine.h"
#include "crt.h"

#if defined(TV_HOOK_BUILD)
#include <windows.h>

/* The SAPI layer's vector push (still the original's code). */
/* @0x100385b0 */
int32_t TV_THISCALL SapiQueue_Push(void *q, const void *data, uint32_t size);

void Sapi_Lock(SapiCentral *s)
{
    EnterCriticalSection((CRITICAL_SECTION *)s->audio_lock);
}

void Sapi_Unlock(SapiCentral *s)
{
    LeaveCriticalSection((CRITICAL_SECTION *)s->audio_lock);
}

void Sapi_Post(SapiCentral *s, uint32_t msg, uint32_t wp, uint32_t lp)
{
    PostMessageA((HWND)s->hwnd, msg, (WPARAM)wp, (LPARAM)lp);
}

int32_t Sapi_QueuePush(SapiCentral *s, const void *data, uint32_t size)
{
    return SapiQueue_Push(s->audio_queue, data, size);
}

#else

void Sapi_Lock(SapiCentral *s) { (void)s; }
void Sapi_Unlock(SapiCentral *s) { (void)s; }
void Sapi_Post(SapiCentral *s, uint32_t msg, uint32_t wp, uint32_t lp)
{
    (void)s; (void)msg; (void)wp; (void)lp;
}
/* Sapi_QueuePush is the library's (src/port/tvtts.c): it is where a
 * bookmark leaves the engine, so the layer that owns the engine terminates
 * it rather than the engine stubbing it out. */

#endif

/*
 * Tell the SAPI layer which phoneme is about to be spoken.
 *
 * The record goes on the same queue as the audio, so the layer can raise the
 * notification when that part of the sound reaches the speaker rather than
 * when it was worked out.  A run of silence is reported once.
 */
/* @0x10031050 */
void TV_THISCALL Sapi_PhoneNotify(Engine *self, int32_t ch)
{
    SapiCentral *s = self->sapi;
    uint8_t c = (uint8_t)ch;
    int32_t *rec;

    if (s == NULL)
        return;
    if (c == ' ') {
        if (self->w_2132 != 0)
            return;
        self->w_2132 = 1;
    } else {
        self->w_2132 = 0;
    }

    rec = (int32_t *)tv_new(12);
    if (rec == NULL)
        return;
    rec[0] = 1;
    rec[1] = 0;
    rec[2] = (int32_t)(int8_t)c;
    Sapi_Lock(s);
    Sapi_QueuePush(s, &rec, sizeof rec);
    Sapi_Unlock(s);
    Sapi_Post(s, 0x4c8, 0, 0);
}
