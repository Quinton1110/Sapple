/* zb_ratechg.h - CRateChanger (vtbl 0x180171280): the engine's own SOLA time-scale modifier, used on the waveform by
 * voices without VoiceSetting.WaveScale=Sonic (OneCore Zira, Mark). See zb_ratechg.c. */
#ifndef ZB_RATECHG_H
#define ZB_RATECHG_H

typedef struct ZbRateChg ZbRateChg;

ZbRateChg *zb_rc_create(int samplerate);
void zb_rc_free(ZbRateChg *r);
/* slot 1: set the speed (> 1 = faster); a changed speed resets the stream (pending samples are dropped) */
void zb_rc_set_speed(ZbRateChg *r, double speed);
/* slot 4: process n input samples; flags bit0 = reset first, bit1 = flush at the end. Output (at most
 * (1/speed + 1) * (pending + n) samples) goes to out; returns the number of output samples. */
int zb_rc_process(ZbRateChg *r, const short *in, int n, short *out, int flags);
/* slot 5: flush (0 samples when nothing is pending) */
int zb_rc_flush(ZbRateChg *r, short *out);
/* upper bound of the output of zb_rc_process(r, .., n, ..) */
long zb_rc_max_out(const ZbRateChg *r, int n);

#endif
