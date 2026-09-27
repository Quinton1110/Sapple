/* Internal glue between the libsam modules. */
#ifndef SAM_INTERNAL_H
#define SAM_INTERNAL_H

#include "sam.h"
#include "sam_lex.h"

struct sam_tts {
    sam_voice *voice;
    sam_lexicon *lex;
    sam_lts *lts;
    sam_synth *synth;
    float base_pitch; /* Hz, from the voice's .sdf */
    int monotone;     /* SAPI 4 RoboSoft: flat pitch */
    uint32_t rand_state; /* MSVC rand(): the engine's accent prominences come from it */
    /* quote / parenthesis state of the word builder (engine this+0xe8/0xe9, pitch offset 0xec,
     * range 0xf0, rate 0x28); kept across sentences like the engine does */
    int in_quote, in_paren;
    float st_off, st_rng, st_rate;
    /* output held back until the engine would have flushed it (see sam_tts_speak) */
    sam_pcm_cb out_cb;
    void *out_user;
    int16_t *pend;
    size_t npend, pcap;
    float transpose; /* sam_params.transpose, for sam_tts_sing */
    /* library hosting (sam_tts_speak_ex): options, running sample count, current sentence span */
    const sam_speak_opts *opts;
    long long ev_total;
    int sent_pos, sent_len;
};

int sam_synth_chunk_pos(const sam_synth *s);
void sam_synth_set_singing(sam_synth *s, int on, float vib_cents, float vib_rate); /* ClassicVoices patch */
void sam_synth_chunk_reset(sam_synth *s);

/* Raw access to a section of the .spd voice file (0..4). */
const uint8_t *sam_voice_section(const sam_voice *v, int index, uint32_t *size);

#endif
