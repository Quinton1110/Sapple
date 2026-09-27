/* eva_voice.h - Microsoft Eva (the Windows 10 / 11 neural "Cortana" voice) as a back end of this engine.
 * ClassicVoices addition (not in the upstream): ported from ~/code/eva-onecore, reconstructed from the Windows 11
 * 26100.1742 MSTTSEngine_OneCore.dll / MSTTSLoc_OneCore.dll.
 *
 * Eva shares the front end (zf1, zf2), her duration model is an ordinary APM (Dur + PDur trees), and her sound comes
 * from the ordinary float LSF vocoder at order 40.  What differs: a neural network (M1033Eva.NNM describes its 631
 * inputs, M1033Eva.TDAT holds 7 layers: int8 dense + 3 projected LSTMs) predicts the spectrum / pitch / loudness /
 * voicing targets every 5 ms, with pitch histogram equalisation (M1033Eva.HEQ), and three CRF models
 * (M1033Eva.BR2 / .TON / .ACL) replace the rule-based phrase breaks, boundary tones and pitch accents. */
#ifndef EVA_VOICE_H
#define EVA_VOICE_H

#include "zb.h"
#include "zf2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EvaModel EvaModel;

/* voicepath = "<dir>/M1033Eva" (.NNM .TDAT .HEQ .BR2 .TON .ACL .INI are read); attaches the prosody models to vf.
 * Also Microsoft Sarah: "<dir>/M2057Sarah", lcid 2057 (.nnm .tdat .heq - Windows' lower-case names are accepted -, no
 * prosody models).  NULL on failure (message in err). */
EvaModel *eva_model_open(const char *voicepath, int lcid, zf2_voice *vf, char *err, int errlen);
void eva_model_close(EvaModel *m);

/* One sentence, the counterpart of zb_synth: vb = the ZbVoice loaded from M1033Eva.APM (durations, vocoder state,
 * INI settings), s / u = zf2_run / zf2_features of the sentence.  Calls write() once with the whole sentence;
 * fills trace->nphone / nstate / nframe / dur (for the host's event timing).  0 = ok, else what write returned / -1. */
int eva_synth(EvaModel *m, ZbVoice *vb, const zf2_sent *s, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write,
              void *user, ZbTrace *trace);

/* debug (Mac tools only): print the per-word prosody labels to stderr; models on/off (off = the rules) */
void eva_model_set_debug(EvaModel *m, int models, int verbose);

#ifdef __cplusplus
}
#endif
#endif
