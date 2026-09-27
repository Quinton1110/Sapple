/* The SAPI 4 Microsoft TTS voice effects (msttssyn.dll, "Whistler 4.0"): the "in Hall", "in Stadium",
 * "in Space" and RoboSoft voices, and the Whisper filter, as a stand-alone audio effect.
 *
 * Works on any mono stream of 16-bit-scale float samples (or int16 via sam4fx_process_s16).
 */
#ifndef SAM4FX_H
#define SAM4FX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the engine's seven presets (msttssyn table at 0x6371f018..) */
enum {
    SAM4FX_NONE = 0,
    SAM4FX_ROBO_B = 1,  /* RoboSoft Two / Five: 10 ms allpass, feedback -0.5 dB */
    SAM4FX_ROBO_C = 2,  /* RoboSoft Three / Four: 10.6 ms allpass, feedback -10.5 dB */
    SAM4FX_HALL = 3,    /* "in Hall": four allpasses 30.6 / 20.83 / 14.85 / 10.98 ms */
    SAM4FX_ROOM = 4,    /* (unused by Microsoft's voices): 81.2 / 55.3 / 35.7 / 21.96 ms */
    SAM4FX_STADIUM = 5, /* "in Stadium": 162.4 / 110.6 / 71.4 / 43.92 ms */
    SAM4FX_SPACE = 6,   /* "in Space": one 400.6 ms allpass */
    SAM4FX_ROBO_A = 7   /* RoboSoft One / Six: like ROBO_B with a randomly shortened loop per call */
};

typedef struct sam4fx sam4fx;

/* preset 1..7 (0 = only the optional FIR); rate = sample rate of the stream */
sam4fx *sam4fx_new(int preset, int rate);
/* optional FIR before the effect (the Whisper voices use {0.25, -0.5, 0.25}) */
int sam4fx_set_fir(sam4fx *fx, const float *taps, int n);
/* scale the input before everything else (the effect is linear, so this only sets the level); default 1 */
void sam4fx_set_input_gain(sam4fx *fx, float g);
/* a trim (in dB) that keeps preset p from clipping on typical 16-bit speech: RoboSoft adds up to +9 dB */
float sam4fx_default_trim_db(int preset);
void sam4fx_process(sam4fx *fx, float *x, int n);          /* in place, 16-bit scale */
void sam4fx_process_s16(sam4fx *fx, int16_t *x, int n);    /* in place, with clipping */
void sam4fx_free(sam4fx *fx);

/* voice-mode name -> preset + flags, e.g. "hall", "space", "robosoft1".."robosoft6", "whisper", "room".
 * monotone: RoboSoft One, Two, Five, Six flatten the pitch; whisper: noise excitation + FIR.
 * Returns 0, or -1 for an unknown name. */
int sam4fx_lookup(const char *name, int *preset, int *monotone, int *whisper);

#ifdef __cplusplus
}
#endif
#endif
