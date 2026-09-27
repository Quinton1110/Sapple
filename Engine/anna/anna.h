/* Microsoft Anna (SAPI5 "TTS20" unit-selection engine) reconstructed in C.
 *
 * Backend: target units from the frontend -> CART candidates -> Viterbi -> decoder reads -> fades -> WSOLA.
 * The voice data (M1033DSK.*) is read at run time and is not part of this code.
 */
#ifndef ANNA_H
#define ANNA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANNA_RATE 16000

/* one target from the frontend (the engine's 0x68-byte UnitSpec, trimmed to what the backend uses) */
typedef struct {
    int type;          /* unit type 1..260, 0 = pause */
    int f[9];          /* target features */
    unsigned ff, bf;   /* front / back phone flags (bit 0x10 enters the join cost) */
    int brk;           /* break level 0..5 (join cost row) */
    int volume;        /* 0..100 */
    int rate;          /* -10..10 */
    int pitch;         /* -10..10 (not implemented: must be 0) */
    int pause_ms;      /* pause length for type 0 (negative = marker pause) */
} anna_unitspec;

typedef void (*anna_pcm_cb)(const int16_t *pcm, size_t n, void *user);

/* compressed-audio access: decode samples [start, start+n) of the recording into out */
typedef struct anna_decoder anna_decoder;
anna_decoder *anna_decoder_open(const char *csd_path, const char *idx_path, const unsigned char key[129], char *err, size_t errlen);
int anna_decoder_read(anna_decoder *d, long start, long n, int16_t *out);
void anna_decoder_close(anna_decoder *d);

typedef struct anna_voice anna_voice;

/* base = voice path + name without extension, e.g. ".../enu-dsk/M1033DSK" */
anna_voice *anna_voice_load(const char *base, anna_decoder *dec, char *err, size_t errlen);
void anna_voice_free(anna_voice *v);
/* needed for <pitch>: path of the installed MSTTSEngine.dll (holds the pitch changer's window table) */
int anna_voice_load_pitch_table(anna_voice *v, const char *path, long offset); /* MSTTSEngine.dll: offset 0xd88 */

/* select units for one sentence (n specs); units[i] = chosen unit index or -1 for pauses */
int anna_select(const anna_voice *v, const anna_unitspec *specs, int n, long *units);

/* render one sentence to PCM; sapi_rate is the SAPI rate (-10..10), the voice's DefaultRate (2) is added */
int anna_render_sentence(anna_voice *v, const anna_unitspec *specs, int n, int sapi_rate, anna_pcm_cb cb, void *user);

typedef struct {
    int sapi_rate;      /* SAPI rate -10..10 (the voice's DefaultRate 2 is added) */
    int allow_fast;     /* extension: allow sapi_rate up to 18 (the engine stops at 10) */
    double rate_frac;   /* ClassicVoices patch: added to sapi_rate before clamping, so the rate need not be a whole
                           SAPI step (3^0.1 = 11.6% apart); 0 = the original engine exactly */
    int sapi_volume;    /* 0..100 */
    int pitch_offset;   /* added to every item's pitch (the engine range is -10..10) */
    volatile const int *cancel; /* checked between chunks: nonzero stops the sentence (returns 1) */
    /* called for every item (UnitSpec index) with its audio position in samples from the sentence start,
       just before the chunk holding it is passed to the PCM callback */
    void (*item_cb)(void *user, int item, long long pos);
    void *user;
} anna_render_opts;

/* returns 0, 1 when cancelled, or -1 on error */
int anna_render_sentence_ex(anna_voice *v, const anna_unitspec *specs, int n, const anna_render_opts *opt, anna_pcm_cb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif
