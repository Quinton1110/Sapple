/* zb.h - portable C99 port of the acoustic backend of the Windows OneCore TTS engine
 * (MSTTSEngine_OneCore.dll 10.3.21207: HTS/SPS voices Microsoft David, Zira, Mark; en-US).
 *
 * Input contract ("the matrix"): the backend reads the utterance only through
 * CTTSFeatureExtractionEngine::Extract (vtbl 0x1801714b8 slot 3, FUN_18005bfc0), which evaluates every APM
 * feature path for every phone of the sentence, plus a few per-phone attributes that the SAPI/SSML parser and
 * the pause-insertion code store on the CTTSPhone (prosody block at phone+0x78, silence flags, word grouping).
 * ZbUtt below holds exactly that, one row per phone including the leading and trailing "sil".
 *
 *  - Feature values are ints. Null (the context does not exist, engine kind -1, value -1) is ZB_NULL (-1).
 *    The engine (FUN_180013ab0) compares a null cell as the integer -1 in '==' and 'in-set' questions and
 *    answers 'no' to every ordering question ('<=' etc.). Real feature values are never negative.
 *    No shipped en-US voice has a string-typed feature.
 *  - The feature set is the union of the three voices' APM feature lists (ZB_F_*, see zb_feature_path[]);
 *    a voice only reads the columns its questions use (David 44, Zira 41, Mark 42 features). A frontend
 *    should fill all ZB_NFEAT columns (unused ones may stay ZB_NULL).
 *
 * Golden files: harness/zbtap.exe writes a tagged binary log (documented in zbtap.c and zb_io.c) with the
 * matrix, the per-phone attributes and every intermediate backend stage. zb_load_zbtap() reads it; the text
 * output of harness/ziraprobe.exe (matrix only) is read by zb_load_ziraprobe().
 *
 * Numerics: bit-exact against the engine requires IEEE single/double arithmetic with no contraction and no
 * excess precision: compile with -ffp-contract=off (gcc/clang, and no -ffast-math), /fp:precise (MSVC), and on
 * x86-32 with SSE2 math (-msse2 -mfpmath=sse, MSVC /arch:SSE2 which is the default). The engine calls the
 * Microsoft UCRT libm (exp, log, pow, powf, sin, cos, sqrt); other libms may differ in the last ulp of a few
 * results (see notes/backend_port.md).
 */
#ifndef ZB_H
#define ZB_H
#ifdef __cplusplus
extern "C" {
#endif

#define ZB_NULL (-1)

/* Feature columns (union of the David/Zira/Mark APM feature paths). Order is fixed: never reorder. */
enum {
    ZB_F_BwPosInSyllable,                  /* Phone.BwPosInSyllable */
    ZB_F_FwPosInSyllable,                  /* Phone.FwPosInSyllable */
    ZB_F_NextNextPhoneIdentity,            /* Phone.NextPhone.NextPhone.PhoneIdentity */
    ZB_F_NextPhoneIdentity,                /* Phone.NextPhone.PhoneIdentity */
    ZB_F_PrevPhoneIdentity,                /* Phone.PrevPhone.PhoneIdentity */
    ZB_F_PrevPrevPhoneIdentity,            /* Phone.PrevPhone.PrevPhone.PhoneIdentity */
    ZB_F_SylBreakIndex,                    /* Phone.Syllable.BreakIndex */
    ZB_F_SylBwPosInPhrase,                 /* Phone.Syllable.BwPosInPhrase */
    ZB_F_SylBwPosInWord,                   /* Phone.Syllable.BwPosInWord */
    ZB_F_SylFwPosInPhrase,                 /* Phone.Syllable.FwPosInPhrase */
    ZB_F_SylFwPosInWord,                   /* Phone.Syllable.FwPosInWord */
    ZB_F_NextSylAccent,                    /* Phone.Syllable.NextSyllable.Accent */
    ZB_F_NextSylBreakIndex,                /* Phone.Syllable.NextSyllable.BreakIndex */
    ZB_F_NextSylPhoneNumber,               /* Phone.Syllable.NextSyllable.PhoneNumber */
    ZB_F_NextSylStress,                    /* Phone.Syllable.NextSyllable.Stress */
    ZB_F_SylPhoneNumber,                   /* Phone.Syllable.PhoneNumber */
    ZB_F_PrevSylAccent,                    /* Phone.Syllable.PrevSyllable.Accent */
    ZB_F_PrevSylBreakIndex,                /* Phone.Syllable.PrevSyllable.BreakIndex */
    ZB_F_PrevSylPhoneNumber,               /* Phone.Syllable.PrevSyllable.PhoneNumber */
    ZB_F_PrevSylStress,                    /* Phone.Syllable.PrevSyllable.Stress */
    ZB_F_SylStress,                        /* Phone.Syllable.Stress */
    ZB_F_WordBwPosInPhrase,                /* Phone.Syllable.Word.BwPosInPhrase */
    ZB_F_WordBwPosInYesNoQuestion,         /* Phone.Syllable.Word.BwPosInYesNoQuestion (David) */
    ZB_F_WordFwPosInPhrase,                /* Phone.Syllable.Word.FwPosInPhrase */
    ZB_F_NextWordPOS,                      /* Phone.Syllable.Word.NextWord.POS */
    ZB_F_NextWordSyllableNumber,           /* Phone.Syllable.Word.NextWord.SyllableNumber */
    ZB_F_PhraseBwPosInSentence,            /* Phone.Syllable.Word.Phrase.BwPosInSentence */
    ZB_F_PhraseFwPosInSentence,            /* Phone.Syllable.Word.Phrase.FwPosInSentence */
    ZB_F_SentPhraseNumber,                 /* Phone.Syllable.Word.Phrase.IntonationPhrase.Sentence.PhraseNumber */
    ZB_F_SentSentenceType,                 /* ...Sentence.SentenceType (David) */
    ZB_F_SentSyllableNumber,               /* ...Sentence.SyllableNumber */
    ZB_F_SentWordNumber,                   /* ...Sentence.WordNumber */
    ZB_F_PhraseSyllableNumber,             /* Phone.Syllable.Word.Phrase.SyllableNumber */
    ZB_F_PhraseWordNumber,                 /* Phone.Syllable.Word.Phrase.WordNumber */
    ZB_F_WordPOS,                          /* Phone.Syllable.Word.POS */
    ZB_F_PrevWordPOS,                      /* Phone.Syllable.Word.PrevWord.POS */
    ZB_F_PrevWordSyllableNumber,           /* Phone.Syllable.Word.PrevWord.SyllableNumber */
    ZB_F_WordSyllableNumber,               /* Phone.Syllable.Word.SyllableNumber */
    ZB_F_PhoneIdentity,                    /* Phone.PhoneIdentity */
    ZB_F_WordFwPosInYesNoQuestion,         /* Phone.Syllable.Word.FwPosInYesNoQuestion (David) */
    ZB_F_NextPhraseSyllableNumber,         /* Phone.Syllable.Word.Phrase.NextPhrase.SyllableNumber */
    ZB_F_NextPhraseWordNumber,             /* Phone.Syllable.Word.Phrase.NextPhrase.WordNumber */
    ZB_F_PrevPhraseSyllableNumber,         /* Phone.Syllable.Word.Phrase.PrevPhrase.SyllableNumber */
    ZB_F_PrevPhraseWordNumber,             /* Phone.Syllable.Word.Phrase.PrevPhrase.WordNumber */
    ZB_F_PhraseToBIFinalBoundaryTone,      /* Phone.Syllable.Word.Phrase.ToBIFinalBoundaryTone (Mark) */
    ZB_F_SylToBIAccent,                    /* Phone.Syllable.ToBIAccent (Eva) */
    ZB_F_PrevSylToBIAccent,                /* Phone.Syllable.PrevSyllable.ToBIAccent (Eva) */
    ZB_F_NextSylToBIAccent,                /* Phone.Syllable.NextSyllable.ToBIAccent (Eva) */
    ZB_NFEAT
};
extern const char *const zb_feature_path[ZB_NFEAT];   /* full dotted APM path per column */

/* Per-phone attributes outside the matrix (CTTSPhone fields the backend reads). Defaults for plain text:
 * volume 100, everything else 0 / "" except: phone_ms on silences and pauses (leading silence 100 ms, trailing silence
 * [SilenceLength] SentenceBoundary = David 750 / Zira, Mark 500, inner pauses their boundary length), is_sil on silence
 * and pause phones, word = word index, sent_type = 3 on the trailing silence of a yes/no question (Zira).
 * SAPI XML: <volume level> -> volume, <rate> -> rate, <pitch> -> pitch, <emph> -> volume 120, rate -4, pitch +4 on the
 * emphasised word (the engine's prosody block; verified with zbtap). */
typedef struct {
    int volume;         /* prosody block +0x00: SAPI <volume> 0..100 (applied per phone to the PCM) */
    int rate;           /* +0x04: SAPI <rate absspeed/speed> -10..10 (clamped to [-20,20]) */
    int pitch;          /* +0x08: SAPI <pitch absmiddle/middle> (clamped to [-10,10]) */
    float pitch_range;  /* +0x0c: pitch range multiplier; 0 = default (1.0) */
    int emph;           /* +0x10: (not read by the acoustic backend; kept for completeness) */
    int group_ms;       /* +0x14: target duration (ms) of the group of phones of this word (second duration pass) */
    int phone_ms;       /* +0x18: target duration (ms) of this phone (pauses/silences: 100 ms before, 750 ms after a
                           David sentence = [SilenceLength], <silence msec>) */
    int is_sil;         /* phone is a silence phone (FUN_18001d540: phone id == sil or pause id) */
    int is_pause;       /* phone is the short-pause phone (FUN_18004d844) */
    int word;           /* index of the owning word (for grouping), -1 none */
    int word_type;      /* word vtbl+0x1b0 (2 normal, 4 spelled/number-like, 5 silence word, 3 skips the pause target);
                           -1 none */
    char domain[32];    /* NUS domain name of the word (segment word vt+0xe0), e.g. "phonenumber" for digits read as a phone
                           number; "" or "none" = no domain. Selects [Domain] RateAdjustment<i> (Zira/Mark phoneNumber 80) */
    int seg_rate;       /* segment+0x40: extra rate steps added to the XML rate on the waveform (0 for plain text) */
    int word90;         /* *(int*)(word+0x90): 1 = the word is exempt from rate adjustments (FUN_18007ae44) */
    int sent_type;      /* sentence type of the utterance's final word node (word+0x228), set on the trailing silence:
                           3 or 5 = yes/no question -> Zira [QuestionIntonation] ramp from the first phone of the last
                           syllable of the last word; 4 = wh-question; 0 = statement */
} ZbPhoneCtl;

typedef struct {
    int f[ZB_NFEAT];    /* feature matrix row, ZB_NULL = null */
    ZbPhoneCtl c;
} ZbPhone;

typedef struct {
    int nphone;
    ZbPhone *ph;
} ZbUtt;

/* ---- emotion / speaking style ([EmotionRecipe] of the voice INI) ----
 *
 * Each of the three en-US OneCore voices carries an [EmotionRecipe] section with the emotions "happy", "sad" and
 * "angry", four percentages each (100 = the voice's normal setting).  The engine selects one through the emotion
 * object of the utterance (CTTSEmotionSetting, INI loader FUN_180016da0, 0x14-byte entries {id, Rate +4, Pitch +8,
 * PitchRangeScale +0xc, Volume +0x10}); the four numbers then enter the pipeline at exactly four places:
 *
 *   rate    FUN_18004e254 (wave-generator rate; decompile: "uVar7 = uVar7 + *(int *)(lVar1 + 4) + -100"):
 *           R% = DomainRate - 100 + Config.RateAdjustment + emotion.RateAdjustment - 100, converted to SAPI rate
 *           steps by round(log3(R/100) * 10 * 1000).  zb folds the same R into the duration-side rate as well
 *           (FUN_18006cda8), which is where David's MixRateScale=1 applies it -- see the note below.
 *   pitch   FUN_18001b100: P = Config.PitchAdjustment + emotion.PitchAdjustment - 100; every F0 of the utterance
 *           is multiplied by P/100 (on top of the SAPI <pitch> factor), clamped to [3, 8000] Hz.
 *   range   FUN_18001b100 / FUN_1800a2810: S = Config.PitchRangeScale + emotion.PitchRangeScale - 100, then
 *           f0 = (f0 - mean) * S/100 * r + mean over the whole utterance (mean = mean of the voiced F0).
 *   volume  FUN_18001fedc: V = (Sps)VolumeAdjustment + emotion.VolumeAdjustment - 100; the PCM of the utterance
 *           is scaled by V/100 (truncating, clipped to +-31000) before the SAPI volume curve.
 *
 * NOT VERIFIED against the engine: no shipped host can ask it for an emotion.  SAPI 5 (SPF_IS_XML and SSML through
 * SAPI's own parser) and the WinRT SpeechSynthesizer (SynthesizeSsmlToStreamAsync) both drop the mstts: namespace,
 * so <mstts:emotion><mstts:category name="happy"/> and <mstts:express-as style=...> give byte-identical audio to
 * plain text on David/Zira/Mark (measured).  The formulas above are read off the decompile; the one inference is
 * that the emotion rate also reaches the duration path (there the engine passes a precomputed rate delta from
 * utterance+0xc8 as FUN_18006cda8's param_8, whose write site was not located).  Folding R in is what makes David
 * -- the only voice with MixRateScale=1 -- change durations instead of time-stretching the waveform, as Zira and
 * Mark do.  Neutral settings (all fields 0) leave every code path bit-identical to the engine.
 */
typedef struct {
    int rate;           /* RateAdjustment %   (0 or 100 = neutral) */
    int pitch;          /* PitchAdjustment %  */
    int range;          /* PitchRangeScale %  */
    int volume;         /* VolumeAdjustment % */
} ZbStyle;

/* SAPI engine-site controls (ISpVoice::SetRate / SetVolume) and the emotion recipe in force. */
typedef struct {
    int rate;           /* -10..10 */
    int volume;         /* 0..100 */
    ZbStyle style;      /* all zero = neutral */
    int rate_milli;     /* ClassicVoices patch: fraction of a rate step on top of everything else, in 1/1000 steps
                           (0..999; 0 = the original engine exactly) - see zira_tts_set_rate_f */
} ZbSapi;

typedef struct ZbVoice ZbVoice;

/* Look up an [EmotionRecipe] entry of the loaded voice by name ("happy", "sad", "angry"; case-insensitive).
 * name == NULL or "" fills *out with zeros (neutral).  Returns 0 on success, -1 if the voice has no such emotion. */
int zb_emotion(const ZbVoice *v, const char *name, ZbStyle *out);
/* Enumerate the voice's emotions: returns the name of entry i, or NULL when i is out of range. */
const char *zb_emotion_name(const ZbVoice *v, int i);

/* Load <voicepath>.APM, .INI (and .BEP). voicepath is e.g. "C:/Windows/Speech_OneCore/Engines/TTS/en-US/M1033David".
 * Returns NULL on failure (message in err). */
ZbVoice *zb_voice_load(const char *voicepath, char *err, int errlen);
void zb_voice_free(ZbVoice *v);
int zb_voice_is_int(const ZbVoice *v);     /* 0 = float path (David), 1 = fixed-point path (Zira, Mark) */
int zb_voice_has_acoustics(const ZbVoice *v);   /* ClassicVoices addition: 0 for Eva's duration-only APM */

/* PCM sink: 16 kHz mono int16. Return 0 to continue. */
typedef int (*zb_write_fn)(void *user, const short *pcm, int nsamples);

/* Optional intermediate results (for tests). Arrays are malloc'ed by zb_synth and freed by zb_trace_free. */
typedef struct {
    int nphone, nstate, nframe, order;
    int *dur;           /* [nphone*nstate] state durations (frames) */
    int *vuv;           /* [nframe] */
    float *lf0;         /* [nframe] log F0 after the F0 predictor (0 = unvoiced); int path: raw ints as float */
    float *lsf;         /* [nframe*order] after the LSF predictor */
    float *gain;        /* [nframe] after the gain predictor */
    float *f0;          /* [nframe] final F0 in Hz (after export/pitch) */
    float *lsf_fin;     /* [nframe*order] final LSF (after export) */
    int *ilf0, *ilsf, *igain, *if0; /* int path equivalents (raw engine integers) */
} ZbTrace;
void zb_trace_free(ZbTrace *t);

/* Synthesize one sentence (utterance). Streams PCM to write(). Returns 0 on success. */
int zb_synth(ZbVoice *v, const ZbUtt *u, const ZbSapi *sapi, zb_write_fn write, void *user, ZbTrace *trace);

/* ---- golden-file loaders (zb_io.c) ---- */
typedef struct {
    int nutt;
    ZbUtt *utt;
    ZbSapi sapi;
    char token[512];
    ZbTrace *ref;       /* [nutt] reference stages from a zbtap log (zeroed for ziraprobe dumps) */
    short **pcm;        /* [nutt] reference PCM per utterance (zbtap), NULL otherwise */
    long *npcm;
} ZbGolden;
int zb_load_zbtap(const char *path, const ZbVoice *v, ZbGolden *g);     /* also keeps reference stages, see zb_io.h */
int zb_load_ziraprobe(const char *path, const ZbVoice *v, ZbGolden *g); /* text dump of ziraprobe.exe (matrix only) */
void zb_golden_free(ZbGolden *g);
int zb_write_wav(const char *path, const short *pcm, long n);

#ifdef __cplusplus
}
#endif
#endif
