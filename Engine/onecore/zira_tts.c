/* Microsoft David / Zira / Mark text to speech: library interface (see zira_tts.h).
 *
 * The pipeline is the same one zira_say.c drives: zf1 (text -> sentences with pronunciations) -> zf2 (prosody,
 * phone tree, feature matrix, SAPI events) -> zb (acoustics, 16 kHz PCM).  This file adds what a host needs:
 * streaming in small chunks, SAPI-style events with UTF-8 byte offsets into the text it was given, a cancel flag
 * another thread may set, and the voice's emotion recipes.
 */
#include "zira_tts.h"

#include "zb.h"
#include "zf.h"
#include "zf2.h"
#include "eva_voice.h"   /* ClassicVoices addition: Microsoft Eva */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEF_DIR "C:/Windows/Speech_OneCore/Engines/TTS/en-US"
#define CHUNK 1600              /* 100 ms: the granularity of streaming and of zira_tts_cancel */

struct zira_tts {
    char dir[1024];
    char voice[32];
    char emotion[32];
    ZbVoice *vb;                /* the selected voice */
    zf2_voice *vf;
    ZbVoice *db;                /* David, loaded on demand for <lang> (see below) */
    zf2_voice *df;
    zf1_engine *e;
    int male;                   /* the voice's gender decides whether <lang> switches to David */
    int lcid;                   /* ClassicVoices patch: the voice's locale (1033 en-US, 2057 en-GB) */
    ZbSapi sapi;                /* engine-site rate/volume (the emotion is filled in per voice at speak time) */
    int rate_ext;               /* fast extension: extra per-phone <rate> steps above site rate 10 */
    int pitch;                  /* offset added to every phone's <pitch middle> */
    volatile int cancel;
    EvaModel *eva;              /* ClassicVoices addition: non-NULL for Microsoft Eva (neural back end) */
};

static int stricmp_ascii(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return x - y;
    }
    return *a - *b;
}

const char *zira_tts_voice(const zira_tts *t) { return t ? t->voice : NULL; }

/* ClassicVoices patch: locale per voice.  The en-US voices are the upstream's; the en-GB ones (Windows 10 / 11 "Hazel",
 * "George", "Susan") use the same engine with the en-GB language data MSTTSLocEnGB.dat and its domain files, which sit
 * next to the en-US files in the same folder (no file names clash).  The locale decides the data file names
 * (M<lcid><Voice>, MSTTSLoc<xx>.dat, the domain .dat files: zf1_open_lcid) and the phone set of the back half
 * (read from the language data: zf2_voice_set_phoneset). */
typedef struct {
    const char *name;
    const char *prefix;
    const char *locale;
    const char *dat;
    int lcid;
    int male;
    int neural;
} zira_voice_def;

static const zira_voice_def VOICES[] = {
    /* ar-SA, ar-EG, bg-BG, ca-ES, cs-CZ, da-DK */
    {"Naayf",      "M1025Naayf",     "ar-SA", "MSTTSLocArSA.dat", 1025, 1, 0},
    {"Hoda",       "M3073Hoda",      "ar-eg", "MSTTSLocArEG.dat", 3073, 0, 0},
    {"Ivan",       "M1026Ivan",      "bg-BG", "MSTTSLocBgBG.dat", 1026, 1, 0},
    {"Herena",     "M1027Herena",    "ca-ES", "MSTTSLocCaES.dat", 1027, 0, 0},
    {"Jakub",      "M1029Jakub",     "cs-CZ", "MSTTSLocCsCZ.dat", 1029, 1, 0},
    {"Helle",      "M1030Helle",     "da-DK", "MSTTSLocDaDK.dat", 1030, 0, 0},
    /* de-AT, de-CH, de-DE */
    {"Michael",    "m3079Michael",   "de-AT", "MSTTSLocDeAT.dat", 3079, 1, 0},
    {"Karsten",    "M2055Karsten",   "de-CH", "MSTTSLocDeCH.dat", 2055, 1, 0},
    {"Hedda",      "M1031Hedda",     "de-DE", "MSTTSLocDeDE.dat", 1031, 0, 0},
    {"Katja",      "M1031Katja",     "de-DE", "MSTTSLocDeDE.dat", 1031, 0, 0},
    {"KatjaV2",    "M1031KatjaV2",   "de-DE", "MSTTSLocDeDE.dat", 1031, 0, 1},
    {"Stefan",     "M1031Stefan",    "de-DE", "MSTTSLocDeDE.dat", 1031, 1, 0},
    /* el-GR */
    {"Stefanos",   "M1032Stefanos",  "el-GR", "MSTTSLocElGR.dat", 1032, 1, 0},
    /* en-AU, en-CA, en-GB, en-IE, en-IN, en-US */
    {"Catherine",  "M3081Catherine", "en-AU", "MSTTSLocEnAU.DAT", 3081, 0, 0},
    {"James",      "M3081James",     "en-AU", "MSTTSLocEnAU.DAT", 3081, 1, 0},
    {"Matilda",    "M3081Matilda",   "en-AU", "MSTTSLocEnAU.DAT", 3081, 0, 1},
    {"EvaCA",      "M4105Eva",       "en-CA", "MSTTSLocEnCA.DAT", 4105, 0, 0},
    {"Linda",      "M4105Linda",     "en-CA", "MSTTSLocEnCA.DAT", 4105, 0, 0},
    {"Richard",    "M4105Richard",   "en-CA", "MSTTSLocEnCA.DAT", 4105, 1, 0},
    {"George",     "M2057George",    "en-GB", "MSTTSLocEnGB.dat", 2057, 1, 0},
    {"Hazel",      "M2057Hazel",     "en-GB", "MSTTSLocEnGB.dat", 2057, 0, 0},
    {"Sarah",      "M2057Sarah",     "en-GB", "MSTTSLocEnGB.dat", 2057, 0, 1},
    {"Susan",      "M2057Susan",     "en-GB", "MSTTSLocEnGB.dat", 2057, 0, 0},
    {"Sean",       "M6153Sean",      "en-IE", "MSTTSLocEnIE.dat", 6153, 1, 0},
    {"Heera",      "M16393Heera",    "en-IN", "MSTTSLocEnIN.dat", 16393, 0, 0},
    {"Koyal",      "M16393Koyal",    "en-IN", "MSTTSLocEnIN.dat", 16393, 0, 0},
    {"Ravi",       "M16393Ravi",     "en-IN", "MSTTSLocEnIN.dat", 16393, 1, 0},
    {"David",      "M1033David",     "en-US", "MSTTSLocEnUS.dat", 1033, 1, 0},
    {"Eva",        "M1033Eva",       "en-US", "MSTTSLocEnUS.dat", 1033, 0, 1},
    {"Mark",       "M1033Mark",      "en-US", "MSTTSLocEnUS.dat", 1033, 1, 0},
    {"Zira",       "M1033Zira",      "en-US", "MSTTSLocEnUS.dat", 1033, 0, 0},
    /* es-ES, es-MX, fi-FI */
    {"AnaES",      "M3082Ana",       "es-ES", "MSTTSLocEsES.dat", 3082, 0, 0},
    {"Helena",     "M3082Helena",    "es-ES", "MSTTSLocEsES.dat", 3082, 0, 0},
    {"Laura",      "M3082Laura",     "es-ES", "MSTTSLocEsES.dat", 3082, 0, 0},
    {"Pablo",      "M3082Pablo",     "es-ES", "MSTTSLocEsES.dat", 3082, 1, 0},
    {"Mila",       "M2058Mila",      "es-MX", "MSTTSLocEsMX.dat", 2058, 0, 1},
    {"Raul",       "M2058Raul",      "es-MX", "MSTTSLocEsMX.dat", 2058, 1, 0},
    {"Sabina",     "M2058Sabina",    "es-MX", "MSTTSLocEsMX.dat", 2058, 0, 0},
    {"Heidi",      "M1035Heidi",     "fi-FI", "MSTTSLocFiFI.dat", 1035, 0, 0},
    /* fr-CA, fr-FR, fr-CH */
    {"Caroline",   "M3084Caroline",  "fr-CA", "MSTTSLocFrCA.DAT", 3084, 0, 0},
    {"Claude",     "M3084Claude",    "fr-CA", "MSTTSLocFrCA.DAT", 3084, 1, 0},
    {"NathalieCA", "M3084Nathalie",  "fr-CA", "MSTTSLocFrCA.DAT", 3084, 0, 0},
    {"Hortense",   "M1036Hortense",  "fr-FR", "MSTTSLocFrFR.dat", 1036, 0, 0},
    {"Julie",      "M1036Julie",     "fr-FR", "MSTTSLocFrFR.dat", 1036, 0, 0},
    {"Nathalie",   "M1036Nathalie",  "fr-FR", "MSTTSLocFrFR.dat", 1036, 0, 1},
    {"Paul",       "M1036Paul",      "fr-FR", "MSTTSLocFrFR.dat", 1036, 1, 0},
    {"Guillaume",  "M4108Guillaume", "fr-ch", "MSTTSLocFrCH.dat", 4108, 1, 0},
    /* he-IL, hi-IN, hr-HR, hu-HU, id-ID */
    {"Asaf",       "M1037Asaf",      "he-IL", "MSTTSLocHeIL.dat", 1037, 1, 0},
    {"Hemant",     "M1081Hemant",    "hi-IN", "MSTTSLocHiIN.dat", 1081, 1, 0},
    {"Kalpana",    "M1081Kalpana",   "hi-IN", "MSTTSLocHiIN.dat", 1081, 0, 0},
    {"Matej",      "M1050Matej",     "hr-HR", "MSTTSLocHrHR.dat", 1050, 1, 0},
    {"Szabolcs",   "M1038Szabolcs",  "hu-HU", "MSTTSLocHuHU.dat", 1038, 1, 0},
    {"Andika",     "M1057Andika",    "id-ID", "MSTTSLocIdID.dat", 1057, 1, 0},
    /* it-IT */
    {"Cosimo",     "M1040Cosimo",    "it-IT", "MSTTSLocItIT.dat", 1040, 1, 0},
    {"Elsa",       "M1040Elsa",      "it-IT", "MSTTSLocItIT.dat", 1040, 0, 0},
    {"ElsaV2",     "M1040ElsaV2",    "it-IT", "MSTTSLocItIT.dat", 1040, 0, 1},
    /* ja-JP, ko-KR, ms-MY, nb-NO, nl-BE, nl-NL */
    {"Ayumi",      "M1041Ayumi",     "ja-JP", "MSTTSLocJaJP.dat", 1041, 0, 0},
    {"Haruka",     "M1041Haruka",    "ja-JP", "MSTTSLocJaJP.dat", 1041, 0, 0},
    {"Ichiro",     "M1041Ichiro",    "ja-JP", "MSTTSLocJaJP.dat", 1041, 1, 0},
    {"Sayaka",     "M1041Sayaka",    "ja-JP", "MSTTSLocJaJP.dat", 1041, 0, 0},
    {"Heami",      "M1042Heami",     "ko-KR", "MSTTSLocKoKR.dat", 1042, 0, 0},
    {"Rizwan",     "M1086Rizwan",    "ms-MY", "MSTTSLocMsMY.dat", 1086, 1, 0},
    {"Jon",        "M1044Jon",       "nb-NO", "MSTTSLocNbNO.dat", 1044, 1, 0},
    {"Bart",       "M2067Bart",      "nl-BE", "MSTTSLocNlBE.dat", 2067, 1, 0},
    {"Frank",      "M1043Frank",     "nl-NL", "MSTTSLocNlNL.dat", 1043, 1, 0},
    /* pl-PL, pt-BR, pt-PT, ro-RO, ru-RU, sk-SK, sl-SI, sv-SE */
    {"Adam",       "M1045Adam",      "pl-PL", "MSTTSLocPlPL.dat", 1045, 1, 0},
    {"Paulina",    "M1045Paulina",   "pl-PL", "MSTTSLocPlPL.dat", 1045, 0, 0},
    {"AnaBR",      "M1046Ana",       "pt-BR", "MSTTSLocPtBR.dat", 1046, 0, 0},
    {"Daniel",     "M1046Daniel",    "pt-BR", "MSTTSLocPtBR.dat", 1046, 1, 0},
    {"Maria",      "M1046Maria",     "pt-BR", "MSTTSLocPtBR.dat", 1046, 0, 0},
    {"Helia",      "M2070Helia",     "pt-PT", "MSTTSLocPtPT.dat", 2070, 0, 0},
    {"Andrei",     "M1048Andrei",    "ro-RO", "MSTTSLocRoRO.dat", 1048, 1, 0},
    {"Irina",      "M1049Irina",     "ru-RU", "MSTTSLocRuRU.dat", 1049, 0, 0},
    {"Pavel",      "M1049Pavel",     "ru-RU", "MSTTSLocRuRU.dat", 1049, 1, 0},
    {"Filip",      "M1051Filip",     "sk-SK", "MSTTSLocSkSK.dat", 1051, 1, 0},
    {"Lado",       "M1060Lado",      "sl-SI", "MSTTSLocSlSI.dat", 1060, 1, 0},
    {"Bengt",      "M1053Bengt",     "sv-SE", "MSTTSLocSvSE.dat", 1053, 1, 0},
    /* ta-IN, th-TH, tr-TR, vi-VN, zh-CN, zh-HK */
    {"Valluvar",   "M1097Valluvar",  "ta-IN", "MSTTSLocTaIN.dat", 1097, 1, 0},
    {"Pattara",    "M1054Pattara",   "th-TH", "MSTTSLocThTH.dat", 1054, 1, 0},
    {"Tolga",      "M1055Tolga",     "tr-TR", "MSTTSLocTrTR.dat", 1055, 1, 0},
    {"An",         "M1066An",        "vi-VN", "MSTTSLocViVN.dat", 1066, 1, 0},
    {"Hongyu",     "M2052Hongyu",    "zh-CN", "MSTTSLocZhCN.dat", 2052, 0, 0},
    {"Huihui",     "M2052Huihui",    "zh-CN", "MSTTSLocZhCN.dat", 2052, 0, 0},
    {"Kangkang",   "M2052Kangkang",  "zh-CN", "MSTTSLocZhCN.dat", 2052, 1, 0},
    {"Yaoyao",     "M2052Yaoyao",    "zh-CN", "MSTTSLocZhCN.dat", 2052, 0, 0},
    {"Danny",      "M3076Danny",     "zh-HK", "MSTTSLocZhHK.dat", 3076, 1, 0},
    {"Tracy",      "M3076Tracy",     "zh-HK", "MSTTSLocZhHK.dat", 3076, 0, 0}
};

static const zira_voice_def *find_voice(const char *voice)
{
    size_t i;
    if (!voice || !*voice) return &VOICES[31]; /* David */
    for (i = 0; i < sizeof VOICES / sizeof *VOICES; i++)
        if (!stricmp_ascii(voice, VOICES[i].name) || !stricmp_ascii(voice, VOICES[i].prefix))
            return &VOICES[i];
    return NULL;
}

int zira_tts_voice_lcid(const char *voice)
{
    const zira_voice_def *vd = find_voice(voice);
    return vd ? vd->lcid : 0;
}

static char g_extra_oc_dir[1024] = "";

void zira_tts_set_extra_dir(const char *extra_dir)
{
    snprintf(g_extra_oc_dir, sizeof g_extra_oc_dir, "%s", extra_dir ? extra_dir : "");
}

static int file_exists_z(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

zira_tts *zira_tts_open(const char *data_dir, const char *voice, char *err, size_t errlen)
{
    zira_tts *t = (zira_tts *)calloc(1, sizeof *t);
    char vpath[1200], dat[1200], testp[1200];
    size_t n;
    const uint8_t *ps;
    const zira_voice_def *vd;
    if (!t) {
        if (err) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(t->dir, sizeof t->dir, "%s", data_dir && *data_dir ? data_dir : DEF_DIR);
    vd = find_voice(voice);
    if (!vd) vd = &VOICES[31]; /* David */
    snprintf(t->voice, sizeof t->voice, "%s", vd->name);
    t->lcid = vd->lcid;
    t->male = vd->male;
    t->sapi.volume = 100;
    snprintf(vpath, sizeof vpath, "%s/%s", t->dir, vd->prefix);
    snprintf(dat, sizeof dat, "%s/%s", t->dir, vd->dat);
    snprintf(testp, sizeof testp, "%s.APM", vpath);
    if (!file_exists_z(testp)) {
        if (g_extra_oc_dir[0]) {
            char test2[1200];
            snprintf(test2, sizeof test2, "%s/%s.APM", g_extra_oc_dir, vd->prefix);
            if (file_exists_z(test2))
                snprintf(vpath, sizeof vpath, "%s/%s", g_extra_oc_dir, vd->prefix);
            else
                snprintf(vpath, sizeof vpath, "%s/%s/%s", t->dir, vd->locale, vd->prefix);
        } else {
            snprintf(vpath, sizeof vpath, "%s/%s/%s", t->dir, vd->locale, vd->prefix);
        }
    }
    if (!file_exists_z(dat)) {
        if (g_extra_oc_dir[0]) {
            char test2[1200];
            snprintf(test2, sizeof test2, "%s/%s", g_extra_oc_dir, vd->dat);
            if (file_exists_z(test2))
                snprintf(dat, sizeof dat, "%s/%s", g_extra_oc_dir, vd->dat);
            else
                snprintf(dat, sizeof dat, "%s/%s/%s", t->dir, vd->locale, vd->dat);
        } else {
            snprintf(dat, sizeof dat, "%s/%s/%s", t->dir, vd->locale, vd->dat);
        }
    }
    t->vb = zb_voice_load(vpath, err, (int)errlen);
    if (!t->vb) goto fail;
    t->vf = zf2_voice_load(vpath);
    t->e = zf1_open_lcid(dat, t->lcid);
    if (!t->vf || !t->e) {
        if (err) snprintf(err, errlen, "cannot load %s.INI or %s", vpath, dat);
        goto fail;
    }
    /* the back half's phone ids are the language data's (en-US: identical to zf2's built-in table) */
    ps = zf1_resource(t->e, 0x29a5584bu, 0x153f1b64u, &n);
    if (zf2_voice_set_phoneset(t->vf, ps, n) < 0) {
        if (err) snprintf(err, errlen, "bad phone set in %s", dat);
        goto fail;
    }
    if (vd->neural) {
        /* ClassicVoices addition: the neural back end (+ Eva's prosody models) */
        char e2[256];
        t->eva = eva_model_open(vpath, t->lcid, t->vf, e2, sizeof e2);
        if (!t->eva) {
            if (err) snprintf(err, errlen, "%s", e2);
            goto fail;
        }
    } else if (!zb_voice_has_acoustics(t->vb)) {
        if (err) snprintf(err, errlen, "%s.APM lacks LSF/LF0 models", vpath);
        goto fail;
    }
    return t;
fail:
    zira_tts_close(t);
    return NULL;
}

void zira_tts_close(zira_tts *t)
{
    if (!t) return;
    if (t->eva) eva_model_close(t->eva);
    if (t->e) zf1_close(t->e);
    if (t->vf) zf2_voice_free(t->vf);
    if (t->vb) zb_voice_free(t->vb);
    if (t->df) zf2_voice_free(t->df);
    if (t->db) zb_voice_free(t->db);
    free(t);
}

/* -10..10 is the engine's engine-site rate.  Above that we keep the site at 10 (where the engine clamps) and put
 * the remainder on every phone's SAPI <rate>, which multiplies by the same 3^(n/10); 20 is then 9x normal. */
void zira_tts_set_rate(zira_tts *t, int rate)
{
    if (rate < -10) rate = -10;
    if (rate > 20) rate = 20;
    t->sapi.rate = rate > 10 ? 10 : rate;
    t->rate_ext = rate > 10 ? rate - 10 : 0;
    t->sapi.rate_milli = 0;   /* ClassicVoices patch (zira_tts_set_rate_f) */
}

/* ClassicVoices patch: fractional rate. The whole part exactly as zira_tts_set_rate, the rest (0..999 thousandths
 * of a step) as ZbSapi.rate_milli. */
void zira_tts_set_rate_f(zira_tts *t, double rate)
{
    long m, whole;
    if (!(rate == rate)) rate = 0;
    if (rate < -10) rate = -10;
    if (rate > 20) rate = 20;
    m = lrint(rate * 1000.0);
    whole = m >= 0 ? m / 1000 : -((-m + 999) / 1000);
    zira_tts_set_rate(t, (int)whole);
    t->sapi.rate_milli = (int)(m - whole * 1000);
}

void zira_tts_set_volume(zira_tts *t, int volume) { t->sapi.volume = volume < 0 ? 0 : volume > 100 ? 100 : volume; }
void zira_tts_set_pitch(zira_tts *t, int pitch) { t->pitch = pitch < -10 ? -10 : pitch > 10 ? 10 : pitch; }
void zira_tts_cancel(zira_tts *t) { t->cancel = 1; }

int zira_tts_set_emotion(zira_tts *t, const char *name)
{
    ZbStyle st;
    if (name && *name && zb_emotion(t->vb, name, &st)) return -1;
    snprintf(t->emotion, sizeof t->emotion, "%s", name ? name : "");
    return 0;
}

const char *zira_tts_emotion_name(const zira_tts *t, int i) { return zb_emotion_name(t->vb, i); }

/* zf1 reports text positions as UTF-16 code units of the string handed to zf1_speak_utf8; map them back to UTF-8
 * byte offsets the same way zf_from_utf8 decodes (a bad byte is one unit). */
static long *utf16_to_utf8_map(const char *s, long *nunits)
{
    size_t len = strlen(s), i = 0;
    long *m = (long *)malloc(sizeof(long) * (2 * len + 2)), n = 0;
    if (!m) return NULL;
    while (i < len) {
        unsigned c = (unsigned char)s[i], cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0, ok = 0, k;
        if (extra) {
            ok = 1;
            cp = c & (extra == 3 ? 7u : extra == 2 ? 15u : 31u);
            for (k = 1; k <= extra; k++) {
                unsigned d = i + (size_t)k < len ? (unsigned char)s[i + (size_t)k] : 0;
                if ((d & 0xc0) != 0x80) ok = 0;
                else cp = (cp << 6) | (d & 0x3f);
            }
        }
        m[n++] = (long)i;
        if (ok) {
            if (cp > 0xffff) m[n++] = (long)i;   /* a surrogate pair is two UTF-16 units */
            i += (size_t)extra + 1;
        } else {
            i++;
        }
    }
    m[n] = (long)len;
    *nunits = n;
    return m;
}

typedef struct {
    zira_tts *t;
    const zira_callbacks *cb;
    uint64_t base;              /* samples handed to the host so far */
    const long *map;            /* UTF-16 unit -> UTF-8 byte, within the current piece */
    long nmap, tbase;           /* tbase = byte offset of the piece in the whole text */
    const zf2_events *ev;       /* events of the sentence being synthesized */
    const ZbTrace *tr;
    int stopped;
    char name[512];
} speak_ctx;

static long map_pos(const speak_ctx *c, long u)
{
    if (u < 0) u = 0;
    if (u > c->nmap) u = c->nmap;
    return c->tbase + c->map[u];
}

static void emit(speak_ctx *c, int type, long upos, long ulen, const char *name)
{
    zira_event e;
    if (!c->cb->event) return;
    e.type = type;
    e.audio_pos = c->base;
    e.text_pos = map_pos(c, upos);
    e.text_len = map_pos(c, upos + ulen) - e.text_pos;
    e.name = name;
    c->cb->event(&e, c->cb->user);
}

static void utf8_of(char *out, size_t outlen, const uint16_t *w)
{
    size_t o = 0;
    int i;
    for (i = 0; w && w[i] && o + 4 < outlen; i++) {
        unsigned u = w[i];
        if (u < 0x80) out[o++] = (char)u;
        else if (u < 0x800) { out[o++] = (char)(0xc0 | u >> 6); out[o++] = (char)(0x80 | (u & 63)); }
        else { out[o++] = (char)(0xe0 | u >> 12); out[o++] = (char)(0x80 | (u >> 6 & 63)); out[o++] = (char)(0x80 | (u & 63)); }
    }
    out[o] = 0;
}

/* hand [pcm, pcm+n) to the host in CHUNK-sample pieces, checking the cancel flag between them */
static void deliver(speak_ctx *c, const short *pcm, long n)
{
    long i;
    for (i = 0; i < n && !c->stopped; ) {
        long k = n - i < CHUNK ? n - i : CHUNK;
        if (c->t->cancel) { c->stopped = 1; break; }
        if (c->cb->audio && c->cb->audio(pcm + i, (size_t)k, c->cb->user)) {
            c->stopped = 1;
            c->t->cancel = 1;
        }
        i += k;
        c->base += (uint64_t)k;
    }
}

/* zb_synth hands over the whole utterance at once; split it at the events' phones (the phone's position in the
 * output is its share of the generated frames, which is what the wave stage's own redistribution does too). */
static int sink(void *user, const short *pcm, int n)
{
    speak_ctx *c = (speak_ctx *)user;
    const ZbTrace *tr = c->tr;
    long *cum = NULL, done = 0;
    int i, p, s;
    if (tr && tr->dur && tr->nframe > 0 && (cum = (long *)malloc(sizeof(long) * ((size_t)tr->nphone + 1))) != NULL) {
        long f = 0;
        for (p = 0; p < tr->nphone; p++) {
            cum[p] = f;
            for (s = 0; s < tr->nstate; s++) f += tr->dur[p * tr->nstate + s];
        }
        cum[tr->nphone] = f;
    }
    for (i = 0; c->ev && i < c->ev->n && !c->stopped; i++) {
        const zf2_event *e = &c->ev->ev[i];
        long at;
        if (e->id != ZF2_EV_WORD && e->id != ZF2_EV_SENTENCE && e->id != ZF2_EV_BOOKMARK) continue;
        at = 0;
        if (cum && e->phone > 0 && e->phone <= tr->nphone && cum[tr->nphone] > 0)
            at = (long)((double)n * (double)cum[e->phone] / (double)cum[tr->nphone] + 0.5);
        if (at > n) at = n;
        if (at > done) { deliver(c, pcm + done, at - done); done = at; }
        if (c->stopped) break;
        if (e->id == ZF2_EV_BOOKMARK) {
            utf8_of(c->name, sizeof c->name, e->str);
            emit(c, ZIRA_EV_BOOKMARK, (long)e->lparam, 0, c->name);
        } else {
            emit(c, e->id == ZF2_EV_WORD ? ZIRA_EV_WORD : ZIRA_EV_SENTENCE, (long)e->lparam, (long)e->wparam, NULL);
        }
    }
    if (!c->stopped && done < n) deliver(c, pcm + done, n - done);
    free(cum);
    return c->stopped;
}

/* set_rate's fast extension and set_pitch, applied the way SAPI's <rate>/<pitch middle> would be */
static void apply_controls(const zira_tts *t, ZbUtt *u)
{
    int p;
    if (!t->rate_ext && !t->pitch) return;
    for (p = 0; p < u->nphone; p++) {
        int r = u->ph[p].c.rate + t->rate_ext, pi = u->ph[p].c.pitch + t->pitch;
        u->ph[p].c.rate = r < -20 ? -20 : r > 20 ? 20 : r;
        u->ph[p].c.pitch = pi < -10 ? -10 : pi > 10 ? 10 : pi;
    }
}

/* one piece of the text, with one voice */
static int speak_piece(zira_tts *t, speak_ctx *c, const char *piece, int xml, ZbVoice *vb, zf2_voice *vf)
{
    zf_sentence s;
    int rc = 0;
    long nmap = 0;
    long *map = utf16_to_utf8_map(piece, &nmap);
    if (!map) return -1;
    c->map = map;
    c->nmap = nmap;
    if (zb_emotion(vb, t->emotion, &t->sapi.style)) memset(&t->sapi.style, 0, sizeof t->sapi.style);
    if (zf1_speak_utf8(t->e, piece, xml) < 0) rc = -1;
    while (!rc && !t->cancel && !c->stopped && zf1_next_sentence(t->e, &s) == 1) {
        ZbUtt u;
        zf2_events ev;
        ZbTrace tr;
        memset(&u, 0, sizeof u);
        memset(&ev, 0, sizeof ev);
        memset(&tr, 0, sizeof tr);
        if (t->eva && vb == t->vb) {
            /* ClassicVoices addition: Eva - zf2_process's steps, keeping the sentence tree for the network's
             * features, then her back end instead of zb_synth */
            zf2_sent *zs = zf2_run(vf, &s);
            if (zs && zf2_features(zs, &u) == 0 && zf2_make_events(zs, &ev) == 0 && u.nphone > 0) {
                apply_controls(t, &u);
                c->ev = &ev;
                c->tr = &tr;
                if (eva_synth(t->eva, vb, zs, &u, &t->sapi, sink, c, &tr) && !c->stopped) rc = -1;
                c->ev = NULL;
                c->tr = NULL;
                zb_trace_free(&tr);
            }
            if (zs) zf2_sent_free(zs);
        } else if (zf2_process(vf, &s, &u, &ev) == 0 && u.nphone > 0) {
            apply_controls(t, &u);
            c->ev = &ev;
            c->tr = &tr;
            if (zb_synth(vb, &u, &t->sapi, sink, c, &tr) && !c->stopped) rc = -1;
            c->ev = NULL;
            c->tr = NULL;
            zb_trace_free(&tr);
        }
        zf2_events_free(&ev);
        zf2_utt_free(&u);
        zf_sentence_free(&s);
    }
    c->map = NULL;
    free(map);
    return rc;
}

int zira_tts_speak(zira_tts *t, const char *utf8, int flags, const zira_callbacks *cb)
{
    speak_ctx c;
    zira_callbacks none;
    char *work, *p;
    int xml = (flags & ZIRA_SPEAK_XML) != 0, rc = 0;
    if (!t || !utf8) return -1;
    if (!cb) {
        memset(&none, 0, sizeof none);
        cb = &none;
    }
    memset(&c, 0, sizeof c);
    c.t = t;
    c.cb = cb;
    t->cancel = 0;
    work = (char *)malloc(strlen(utf8) + 1);
    if (!work) return -1;
    strcpy(work, utf8);
    p = work;
    /* SAPI hands <lang langid="..."> ... </lang> to the language's default voice as a separate job, keeping the
       gender: a male voice switches to David, Zira (female) stays herself.  Same rule as zira_say.c. */
    while (*p && !rc && !t->cancel && !c.stopped) {
        /* ClassicVoices patch: only the en-US voices switch to David (his phone set is en-US's) */
        char *tag = xml && t->male && t->lcid == 1033 ? strstr(p, "<lang") : NULL, *piece = p, *end = NULL;
        int david = 0;
        if (tag == p) {
            char *gt = strchr(p, '>'), *close = gt ? strstr(gt, "</lang>") : NULL;
            if (gt && close) {
                *close = 0;
                piece = gt + 1;
                end = close + 7;
                david = 1;
            }
        }
        if (!david) {
            if (tag) { *tag = 0; end = tag; }
            else end = p + strlen(p);
        }
        if (david && !t->db) {
            char dp[1200], err[256];
            snprintf(dp, sizeof dp, "%s/M1033David", t->dir);
            t->db = zb_voice_load(dp, err, sizeof err);
            t->df = zf2_voice_load(dp);
            if (t->df) {
                size_t n;
                const uint8_t *ps = zf1_resource(t->e, 0x29a5584bu, 0x153f1b64u, &n);
                zf2_voice_set_phoneset(t->df, ps, n);
            }
        }
        if (*piece) {
            ZbVoice *ub = david && t->db ? t->db : t->vb;
            zf2_voice *uf = david && t->df ? t->df : t->vf;
            c.tbase = (long)(piece - work);
            rc = speak_piece(t, &c, piece, xml, ub, uf);
        }
        if (end == tag && tag) { *tag = '<'; p = tag; }   /* restore the '<' we cut at */
        else p = end;
    }
    free(work);
    if (rc == 0 && (t->cancel || c.stopped)) rc = 1;
    if (rc == 0 && cb->event) {
        zira_event e;
        memset(&e, 0, sizeof e);
        e.type = ZIRA_EV_END;
        e.audio_pos = c.base;
        e.text_pos = (long)strlen(utf8);
        cb->event(&e, cb->user);
    }
    return rc;
}

/* ClassicVoices addition (Mac tools): Eva's prosody models on / off (the rules decide) and label printing */
int zira_tts_eva_debug(zira_tts *t, int models, int verbose)
{
    if (!t || !t->eva) return -1;
    eva_model_set_debug(t->eva, models, verbose);
    return 0;
}
