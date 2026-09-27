/* zf2_int.h - internal structures of the zf2 frontend stage (see zf2.h, notes/fe2.md). */
#ifndef ZF2_INT_H
#define ZF2_INT_H
#include <stdint.h>
#include "zf2.h"

/* phone set (MSTTSLocEnUS.dat resource 29a5584b, verified): ids 1 '&', 2 '-', 3 -SIL-, 4 -SP-, 5 '1', 6 '2', 7.. */
#define Z2_PH_AMP 1
#define Z2_PH_SYLB 2
#define Z2_PH_SIL 3
#define Z2_PH_SP 4
#define Z2_PH_ST1 5
#define Z2_PH_ST2 6
#define Z2_MAXPH 48
extern const char *const z2_phone_name[Z2_MAXPH];
extern const uint32_t z2_phone_flags[Z2_MAXPH];

/* ClassicVoices patch: the phone set is per voice (zf2_voice.ps).  Default: the built-in en-US table above; zira_tts
 * replaces it with the language data's own (zf2_voice_set_phoneset), which for MSTTSLocEnUS.dat is identical.
 * en-GB (MSTTSLocEnGB.dat) has 49 phones, no -SP-, and its stress / tone marks are ids 4 / 5 instead of 5 / 6. */
#define Z2_PS_MAX 64
typedef struct {
    char name[Z2_PS_MAX][24];   /* "" = no phone with that id */
    uint32_t flags[Z2_PS_MAX];
    int sil;                    /* id of -SIL- (3 in both) */
    int st1;                    /* id of the stress mark '1' */
    int sp;                     /* id of -SP-, -1 if the set has none */
} z2_phoneset;
int z2_phone_id(const z2_phoneset *ps, const char *name, int len);       /* -1 unknown */

/* pause classes (index into the SilenceLength table 0x180170f00) */
enum { Z2_PC_WORD, Z2_PC_INTERM, Z2_PC_PUNCINTERM, Z2_PC_NONPUNCIP, Z2_PC_PARALIP, Z2_PC_PARALLISTIP, Z2_PC_IP, Z2_PC_SENT };

struct zf2_voice {
    int sil[8];             /* SilenceLength.* (ms) in pause-class order */
    int speak_start;        /* SilenceLength.SpeakSessionStart (min 5) = settings+0x2c */
    int speak_end;          /* SilenceLength.SpeakSessionEnd (min 5)   = settings+0x28 */
    int ratio_ne;           /* SilenceLength.RatioNameEntities (100)   = settings+0x30 */
    int punc_interm_raw;    /* SilenceLength.PuncIntermPhraseBoundary as read (10000 = "not configured") */
    int short_pause;        /* ShortPause.Enable -> phone set SP id (4) or -1 */
    int short_pause_on;     /* ClassicVoices patch: ShortPause.Enable as read (short_pause follows the phone set) */
    z2_phoneset ps;         /* ClassicVoices patch: the voice's phone set */
    int no_pause;           /* MSTTS.NoPause */
    /* unit generator / SAPI */
    int normal_factor;      /* VoiceSetting.MSTTS_SpeakRate_NormalFactor */
    int phoneme_events_native; /* VoiceSetting.TtsPhonemeEvents */
    char name[64];
    /* statistical prosody models (Eva only; NULL for David/Zira/Mark): called after the break rules (1), the
     * boundary-tone rules (2) and the pitch-accent rules (3), where the engine runs its model runners */
    void (*prosody_hook)(void *ctx, void *sent, int stage);
    void *prosody_ctx;
};

/* ---- the tree ---- */
typedef struct {
    int type;               /* +0x88 */
    int lang;               /* +0x80 */
    int pos, tpos;          /* +0x138, +0x13a (0xFFFF none) */
    int i180, i210, i214, i228, i22c, i230, i234, i240, i248;
    int tone, tone_lock, bi, bi_lock, pause, emph, emph_lock;
    int src_off, src_len, i278, i27c, i2b8, i2bc;
    int group, attached, ne_obj, has_ne;
    int frag;               /* index into z2sent.frag */
    int pron_null;          /* vt+0x90 returns NULL (no pronunciation object) */
    int npron;
    uint16_t *pron;         /* phone-id string incl. '-' and stress ids */
    const zf_char *text;
    const zf_char *domain;  /* +0x1b0 entity domain ("phonenumber", "none", ...): the backend's NUS/rate domain */
    /* tree links (rebuilt) */
    int s0, ns;             /* syllables [s0, s0+ns) */
    int phrase, pw;         /* -1 none */
    int inserted;           /* 1 = pause word created by zf2 (not in the input) */
    int bookmark;           /* +0x2f0 bookmark attached (SAPI <bookmark> before this word) */
    const zf_char *bookmark_name; /* +0x2f8 */
} Z2W;

typedef struct { int word, p0, np, tobi; } Z2S;
typedef struct { int syl; int id; int st; int tone; } Z2P;
typedef struct { int w0, w1, ip, pw0, pw1; } Z2PH;
typedef struct { int ph0, ph1; } Z2IP;
typedef struct { int w0, w1, ph; } Z2PW;

struct zf2_sent {
    const zf2_voice *v;
    int type, emotion, src_off, src_len;
    int nw, capw; Z2W *w;
    int ns, caps; Z2S *s;
    int np, capp; Z2P *p;
    int nph, capph; Z2PH *ph;
    int nip, capip; Z2IP *ip;
    int npw, cappw; Z2PW *pw;
    int nfrag; zf_fragstate *frag;
    int nquotes; struct zf_quote *quotes;
    int npar; struct zf_parstruct *par;
    int first_ip, last_ip;  /* sentence+0x60/+0x68 */
    int rebuild_flag;       /* word list +0x60 */
};
typedef struct zf2_sent Z2Sent;

/* tree helpers (zf2_tree.c) */
int z2_is_break(const Z2W *w);                     /* vt+0x270: type == 3 */
int z2_is_sapi_silence(const Z2Sent *s, const Z2W *w); /* vt+0x278 */
int z2_pron_len(const Z2W *w);                     /* FUN_18001a1d4 */
int z2_is_rw(const Z2W *w);                        /* FUN_18001a144: (type&~4)==0 && pron */
int z2_has_syl_pron(const Z2W *w);                 /* FUN_18001a56c */
int z2_is_empty_word(const Z2W *w);                /* FUN_180019e34 */
int z2_word_skip(const Z2W *w);                    /* FUN_18001a070 (word nav skip) */
int z2_prev_rw(const Z2Sent *s, int i);            /* FUN_180059d0c */
int z2_next_sp(const Z2Sent *s, int i);            /* FUN_18004620c */
int z2_prev_sp(const Z2Sent *s, int i);            /* FUN_1800b6688 */
int z2_phone_is_sil(const Z2Sent *s, int id);      /* FUN_18001d540 */
int z2_syl_is_sil(const Z2Sent *s, int si);        /* FUN_18001a40c */
int z2_phrase_is_break(const Z2Sent *s, int phi);  /* FUN_1800520d4 */
int z2_ip_is_break(const Z2Sent *s, int ipi);      /* FUN_18014c4b0 */
int z2_insert_word(Z2Sent *s, int at, const Z2W *w);
void z2_rebuild(Z2Sent *s);                        /* FUN_18001a644 */
void z2_build_phrases(Z2Sent *s, int p2);          /* FUN_180045c80 */
void z2_set_tobi_accent(Z2Sent *s, int wi);        /* FUN_180019470 */

void z2_fill_ctl(const Z2Sent *s, ZbUtt *u);        /* zf2_units.c */

/* prosody (zf2_prosody.c) */
void z2_prosody(Z2Sent *s);

#endif
