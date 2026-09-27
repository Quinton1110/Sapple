/* zf2_main.c - voice settings, zf_sentence -> tree conversion, the zf2 pipeline entry points, debug dump. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include "zf2_int.h"

/* ---- voice INI ([Section] Key=Value, keys addressed as "Section.Key"; engine defaults 0x18016d790) ---- */
static int ini_get(const char *ini, const char *sec, const char *key, char *out, int outlen)
{
    const char *p = ini;
    char cur[128] = "";
    while (*p) {
        const char *e = p, *a, *b;
        while (*e && *e != '\n') e++;
        a = p;
        b = e;
        while (a < b && (unsigned char)*a <= ' ') a++;
        while (b > a && (unsigned char)b[-1] <= ' ') b--;
        if (a < b && *a == '[') {
            const char *c = memchr(a, ']', (size_t)(b - a));
            if (c) {
                size_t n = (size_t)(c - a - 1);
                if (n >= sizeof cur) n = sizeof cur - 1;
                memcpy(cur, a + 1, n);
                cur[n] = 0;
            }
        } else if (a < b && *a != ';') {
            const char *eq = memchr(a, '=', (size_t)(b - a));
            if (eq) {
                const char *ke = eq, *v = eq + 1;
                while (ke > a && (unsigned char)ke[-1] <= ' ') ke--;
                while (v < b && (unsigned char)*v <= ' ') v++;
                if ((size_t)(ke - a) == strlen(key) && !strncmp(a, key, (size_t)(ke - a))
#ifdef _WIN32
                    && !_stricmp(cur, sec)
#else
                    && !strcasecmp(cur, sec)
#endif
                ) {
                    int n = (int)(b - v);
                    if (n >= outlen) n = outlen - 1;
                    memcpy(out, v, (size_t)n);
                    out[n] = 0;
                    return 1;
                }
            }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}
static int ini_int(const char *ini, const char *sec, const char *key, int def)
{
    char b[64];
    return ini && ini_get(ini, sec, key, b, sizeof b) ? atoi(b) : def;
}
static int ini_bool(const char *ini, const char *sec, const char *key, int def)
{
    char b[64];
    if (!ini || !ini_get(ini, sec, key, b, sizeof b)) return def;
    return b[0] == 't' || b[0] == 'T' || b[0] == 'y' || b[0] == 'Y' || b[0] == '1';
}

zf2_voice *zf2_voice_load(const char *voicepath)
{
    static const char *const silkey[8] = {"WordBoundary", "IntermPhraseBoundary", "PuncIntermPhraseBoundary",
                                          "NonPuncIntonationPhraseBoundary", "ParalStructIntonationPhraseBoundary",
                                          "ParalStructListIntonationPhraseBoundary", "IntonationPhraseBoundary",
                                          "SentenceBoundary"};
    static const int sildef[8] = {0, 100, 10000, 70, 120, 350, 500, 750};
    zf2_voice *v = (zf2_voice *)calloc(1, sizeof *v);
    char *ini = NULL, path[1024];
    FILE *f;
    int k;
    if (!v) return NULL;
    snprintf(path, sizeof path, "%s.INI", voicepath ? voicepath : "");
    f = fopen(path, "rb");
    if (f) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        ini = (char *)malloc((size_t)n + 1);
        n = (long)fread(ini, 1, (size_t)n, f);
        ini[n] = 0;
        fclose(f);
    }
    for (k = 0; k < 8; k++) v->sil[k] = ini_int(ini, "SilenceLength", silkey[k], sildef[k]);
    v->punc_interm_raw = v->sil[Z2_PC_PUNCINTERM];
    v->speak_start = ini_int(ini, "SilenceLength", "SpeakSessionStart", 100);
    v->speak_end = ini_int(ini, "SilenceLength", "SpeakSessionEnd", 750);
    if (v->speak_start < 5) v->speak_start = 5;
    if (v->speak_end < 5) v->speak_end = 5;
    v->ratio_ne = ini_int(ini, "SilenceLength", "RatioNameEntities", 100);
    {   /* ClassicVoices patch: start from the built-in en-US phone set (see zf2_voice_set_phoneset) */
        int i;
        for (i = 0; i < Z2_MAXPH; i++) {
            snprintf(v->ps.name[i], sizeof v->ps.name[i], "%s", z2_phone_name[i]);
            v->ps.flags[i] = z2_phone_flags[i];
        }
        v->ps.sil = Z2_PH_SIL;
        v->ps.st1 = Z2_PH_ST1;
        v->ps.sp = Z2_PH_SP;
    }
    v->short_pause_on = ini_bool(ini, "ShortPause", "Enable", 0);
    v->short_pause = v->short_pause_on ? v->ps.sp : -1;
    v->no_pause = ini_int(ini, "MSTTS", "NoPause", 0);
    v->normal_factor = ini_int(ini, "VoiceSetting", "MSTTS_SpeakRate_NormalFactor", 100);
    v->phoneme_events_native = ini_int(ini, "VoiceSetting", "TtsPhonemeEvents", 0);
    {
        const char *b = voicepath ? strrchr(voicepath, '/') : NULL, *b2 = voicepath ? strrchr(voicepath, '\\') : NULL;
        if (b2 > b) b = b2;
        snprintf(v->name, sizeof v->name, "%s", b ? b + 1 : voicepath ? voicepath : "");
    }
    free(ini);
    return v;
}
void zf2_voice_free(zf2_voice *v) { free(v); }

/* ---- conversion ---- */
static int zlen(const zf_char *s) { int n = 0; if (s) while (s[n]) n++; return n; }

/* ClassicVoices patch: the phone set from the language data (resource 29a5584b: u32 record size, u32 count, records of
 * {u16 id, 23 x u16 name, u32 flags at +48}; the layout zf1_phoneset_init reads) */
int zf2_voice_set_phoneset(zf2_voice *v, const uint8_t *res, size_t size)
{
    z2_phoneset ps;
    uint32_t rs, n, k;
    if (!v || !res || size < 8) return -1;
    rs = (uint32_t)res[0] | (uint32_t)res[1] << 8 | (uint32_t)res[2] << 16 | (uint32_t)res[3] << 24;
    n = (uint32_t)res[4] | (uint32_t)res[5] << 8 | (uint32_t)res[6] << 16 | (uint32_t)res[7] << 24;
    if (rs < 52 || n == 0 || n > Z2_PS_MAX || 8 + (size_t)rs * n > size) return -1;
    memset(&ps, 0, sizeof ps);
    ps.sil = ps.st1 = ps.sp = -1;
    for (k = 0; k < n; k++) {
        const uint8_t *r = res + 8 + (size_t)rs * k;
        unsigned id = (unsigned)r[0] | (unsigned)r[1] << 8;
        int j;
        if (id == 0 || id >= Z2_PS_MAX) return -1;
        for (j = 0; j < 23; j++) {
            unsigned c = (unsigned)r[2 + 2 * j] | (unsigned)r[3 + 2 * j] << 8;
            if (!c) break;
            ps.name[id][j] = (char)c;
        }
        ps.name[id][j] = 0;
        ps.flags[id] = (uint32_t)r[48] | (uint32_t)r[49] << 8 | (uint32_t)r[50] << 16 | (uint32_t)r[51] << 24;
        if (!strcmp(ps.name[id], "-SIL-")) ps.sil = (int)id;
        else if (!strcmp(ps.name[id], "1")) ps.st1 = (int)id;
        else if (!strcmp(ps.name[id], "-SP-")) ps.sp = (int)id;
    }
    if (ps.sil < 0 || ps.st1 < 0) return -1;
    v->ps = ps;
    v->short_pause = v->short_pause_on ? v->ps.sp : -1;
    return 0;
}

static void conv_word(const zf2_voice *v, Z2W *d, const zf_word *w, int fragi)
{
    memset(d, 0, sizeof *d);
    d->type = w->type;
    d->lang = w->lang;
    d->pos = w->pos;
    d->tpos = w->tpos;
    d->i180 = w->i180;
    d->i210 = w->silence_ms;
    d->i214 = w->i214;
    d->i228 = w->i228;
    d->i22c = w->i22c;
    d->i230 = w->i230;
    d->i234 = w->i234;
    d->i240 = w->i240;
    d->i248 = w->i248;
    d->tone = w->tone;
    d->tone_lock = w->tone_lock;
    d->bi = w->bi;
    d->bi_lock = w->bi_lock;
    d->pause = w->pause_class;
    d->emph = w->emphasis;
    d->emph_lock = w->emph_lock;
    d->src_off = w->src_off;
    d->src_len = w->src_len;
    d->i278 = w->i278;
    d->i27c = w->i27c;
    d->i2b8 = w->i2b8;
    d->i2bc = w->i2bc;
    d->group = w->group;
    d->attached = w->attached_pause;
    d->ne_obj = w->ne_obj;
    d->has_ne = w->ne_type && w->ne_type[0];
    d->frag = fragi;
    d->text = w->text;
    d->domain = w->s1b0;
    d->bookmark = w->i2f0;
    d->bookmark_name = w->s2f8;
    d->phrase = d->pw = -1;
    d->s0 = -1;
    /* current pronunciation: the selected id string, else the phone-name string */
    if (w->nprons > 0 && w->cur_pron >= 0 && w->cur_pron < w->nprons && w->prons[w->cur_pron]) {
        int n = zlen(w->prons[w->cur_pron]), k;
        d->pron = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(n + 1));
        for (k = 0; k < n; k++) d->pron[k] = w->prons[w->cur_pron][k];
        d->npron = n;
    } else if (w->pron && w->pron[0]) {
        int n = zlen(w->pron), k = 0, m = 0;
        d->pron = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(n + 1));
        while (k < n) {
            char nm[16];
            int j = 0, id;
            while (k < n && w->pron[k] == ' ') k++;
            while (k < n && w->pron[k] != ' ' && j < 15) nm[j++] = (char)w->pron[k++];
            if (!j) break;
            id = z2_phone_id(&v->ps, nm, j);
            if (id > 0) d->pron[m++] = (uint16_t)id;
        }
        d->npron = m;
    } else
        d->pron_null = 1;
}

zf2_sent *zf2_run(const zf2_voice *v, const zf_sentence *in)
{
    Z2Sent *s = (Z2Sent *)calloc(1, sizeof *s);
    int i, q;
    if (!s) return NULL;
    s->v = v;
    s->type = in->type;
    s->emotion = in->emotion;
    s->src_off = in->src_off;
    s->src_len = in->src_len;
    s->nw = in->nwords;
    s->capw = in->nwords + 16;
    s->w = (Z2W *)calloc((size_t)s->capw, sizeof(Z2W));
    s->frag = (zf_fragstate *)calloc((size_t)(in->nwords + 1), sizeof(zf_fragstate));
    for (i = 0; i < in->nwords; i++) {
        /* words of one SAPI fragment share one fragment object; identical copies are merged */
        int f;
        for (f = 0; f < s->nfrag; f++)
            if (!memcmp(&s->frag[f], &in->words[i].fs, sizeof(zf_fragstate))) break;
        if (f == s->nfrag) s->frag[s->nfrag++] = in->words[i].fs;
        conv_word(v, &s->w[i], &in->words[i], f);
    }
    s->nquotes = in->nquotes;
    if (in->nquotes) {
        s->quotes = (struct zf_quote *)malloc(sizeof *s->quotes * (size_t)in->nquotes);
        memcpy(s->quotes, in->quotes, sizeof *s->quotes * (size_t)in->nquotes);
    }
    s->npar = in->npar;
    if (in->npar) {
        s->par = (struct zf_parstruct *)calloc((size_t)in->npar, sizeof *s->par);
        for (q = 0; q < in->npar; q++) {
            s->par[q].nel = in->par[q].nel;
            s->par[q].el = (struct zf_parel *)malloc(sizeof(struct zf_parel) * (size_t)(in->par[q].nel + 1));
            memcpy(s->par[q].el, in->par[q].el, sizeof(struct zf_parel) * (size_t)in->par[q].nel);
        }
    }
    s->first_ip = s->last_ip = -1;
    /* the tree as CTextProcessor left it (syllables/phones from the pronunciations, no new pauses) */
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        w->s0 = -1;
        w->ns = 0;
    }
    {
        /* reuse the rebuild's builders without pause insertion: temporarily mark every word as attached-less and
         * no silence -> simplest: a dedicated pass */
        extern void z2_initial_build(Z2Sent *s);
        z2_initial_build(s);
    }
    z2_prosody(s);                  /* CLinguisticProsodyTagger */
    z2_rebuild(s);                  /* CTTSUnitGenerator: sentence vt+0xa0 */
    z2_build_phrases(s, 1);         /*                    sentence vt+0xa8(1) */
    return s;
}

void zf2_sent_free(zf2_sent *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->nw; i++) free(s->w[i].pron);
    for (i = 0; i < s->npar; i++) free(s->par[i].el);
    free(s->par);
    free(s->quotes);
    free(s->w);
    free(s->s);
    free(s->p);
    free(s->ph);
    free(s->ip);
    free(s->pw);
    free(s->frag);
    free(s);
}

int zf2_process(const zf2_voice *v, const zf_sentence *in, ZbUtt *out, zf2_events *ev)
{
    zf2_sent *s = zf2_run(v, in);
    int r;
    if (!s) return -1;
    r = zf2_features(s, out);
    if (!r && ev) r = zf2_make_events(s, ev);
    zf2_sent_free(s);
    return r;
}

/* ---- per-word prosody (see zf2.h): what the rules decided, for callers driving another back end ---- */
int zf2_word_prosody(const zf2_sent *s, const zf2_voice *v, zf2_wordinfo *out, int max)
{
    int i, n;
    if (!s || !out) return -1;
    n = s->nw < max ? s->nw : max;
    for (i = 0; i < n; i++) {
        const Z2W *w = &s->w[i];
        out[i].inserted = w->inserted;
        out[i].type = w->type;
        out[i].bi = w->bi;
        out[i].tone = w->tone;
        out[i].pause_class = w->pause;
        out[i].pause_ms = v && w->pause >= 0 && w->pause < 8 ? v->sil[w->pause] : 0;
        out[i].emphasis = w->emph;
    }
    return n;
}

/* ---- debug dump (subset of the harness/zftap2 format) ---- */
void zf2_dump(const zf2_sent *s, void *fp, const char *stage)
{
    FILE *f = (FILE *)fp;
    int i;
    fprintf(f, "STAGE %s\nSENT type=%d nw=%d ns=%d np=%d nph=%d nip=%d\n", stage, s->type, s->nw, s->ns, s->np, s->nph,
            s->nip);
    for (i = 0; i < s->nw; i++) {
        const Z2W *w = &s->w[i];
        fprintf(f, "W %d i88=%d i210=%d i228=%d i22c=%d i230=%d i238=%d i240=%d i244=%d i24c=%d i250=%d i254=%d i258=%d s=%d-%d ph=%d\n",
                i, w->type, w->i210, w->i228, w->i22c, w->i230, w->tone, w->i240, w->bi, w->bi_lock, w->pause, w->emph,
                w->emph_lock, w->ns ? w->s0 : -1, w->ns ? w->s0 + w->ns - 1 : -1, w->phrase);
    }
    for (i = 0; i < s->ns; i++)
        fprintf(f, "Y %d w=%d ph=%d-%d c0=%d\n", i, s->s[i].word, s->s[i].p0, s->s[i].p0 + s->s[i].np - 1, s->s[i].tobi);
    for (i = 0; i < s->np; i++) fprintf(f, "H %d s=%d id=%d st=%d\n", i, s->p[i].syl, s->p[i].id, s->p[i].st);
    for (i = 0; i < s->nph; i++) fprintf(f, "PH %d %d %d par=%d\n", i, s->ph[i].w0, s->ph[i].w1, s->ph[i].ip);
    for (i = 0; i < s->nip; i++) fprintf(f, "IP %d %d %d\n", i, s->ip[i].ph0, s->ip[i].ph1);
}

void zf2_voice_set_prosody_hook(zf2_voice *v, void (*fn)(void *ctx, void *sent, int stage), void *ctx)
{
    if (!v) return;
    v->prosody_hook = fn;
    v->prosody_ctx = ctx;
}
