/* zb_sonic.h - the engine's embedded Sonic time-scale modifier (see zb_sonic.c). */
#ifndef ZB_SONIC_H
#define ZB_SONIC_H
typedef struct ZbSonic ZbSonic;
ZbSonic *zb_sonic_create(int samplerate);
void zb_sonic_free(ZbSonic *s);
void zb_sonic_set_speed(ZbSonic *s, float speed);
void zb_sonic_write(ZbSonic *s, const short *x, int n);   /* sonicWriteShortToStream */
void zb_sonic_flush(ZbSonic *s);                           /* sonicFlushStream */
int zb_sonic_available(const ZbSonic *s);
int zb_sonic_read(ZbSonic *s, short *dst, int max);        /* sonicReadShortFromStream */
#endif
