/* Microsoft Sam / Mike / Mary text to speech: library interface (see sam_tts.h).
 *
 * This is the hosting layer only: it loads a voice, turns SAPI XML into plain runs of text, drives
 * sam_tts_speak_ex() in sam_front.c and turns its marks into sentence / word / bookmark events with
 * UTF-8 byte offsets into the text the caller gave us. The synthesis itself is untouched: with the
 * default rate, volume and pitch the samples are the ones sam_say writes.
 */
#include "sam_tts.h"
#include "sam.h"
#include "sam4fx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* spttseng's SAPI-rate table (@5ed31978): the duration divisor is 3^(rate/10) */
static const float RATE_TAB[19] = {1.f,          1.1161232f,  1.245731f,   1.3903892f,  1.55184555f, 1.73205078f,
                                   1.933182f,    2.15766931f, 2.40822458f, 2.68787527f, 3.f,         3.3483696f,
                                   3.73719287f,  4.17116737f, 4.65553665f, 5.19615221f, 5.79954624f, 6.47300768f,
                                   7.22467422f};

static double rate_factor(int r)
{
    if (r == 0) return 1.0;
    if (r < 0) return 1.0 / (double)RATE_TAB[-r > 18 ? 18 : -r];
    return (double)RATE_TAB[r > 18 ? 18 : r];
}

/* ------------------------------------------------------------------ the handle */

struct sam_speech {
    sam_tts *eng;
    char spd[1024], lex[1024], lts[1024];
    sam_params p;
    int rate, volume, pitch;
    int fx_preset, fx_on;
    /* ClassicVoices patch: continuous rate / pitch for hosts (see sam_tts_set_rate_pitch_f) */
    int use_f;
    double rate_f, pitch_st;
    int fx_tail_ms; /* ClassicVoices patch: echo tail after the text, faded out (-1 = upstream: 1 s, unfaded) */
    size_t held_zeros; /* ClassicVoices patch: trailing exact-zero samples of the dry signal, not yet delivered */
    volatile int cancel;
    /* per speak call */
    const sam_callbacks *cb;
    sam4fx *fx;
    uint64_t base;      /* samples delivered so far */
    uint64_t run_base;  /* ... before the run being spoken (marks count from there) */
    int stopped;
    const char *orig;   /* the text the caller gave us */
    const long *bmap;   /* stripped byte -> original byte (NULL = identity) */
    long run_start;     /* offset of the current run in the stripped text */
    const long *map;    /* UTF-16 unit -> byte, inside the current run */
    long nmap;
    long last_word;
    struct xdoc *doc;   /* bookmarks */
    int next_bm;
};

static int load_engine(sam_speech *t, char *err, size_t errlen)
{
    sam_tts_free(t->eng);
    t->eng = sam_tts_new(t->spd, t->lex, t->lts, &t->p, err, errlen);
    return t->eng ? 0 : -1;
}

sam_speech *sam_tts_open(const char *dir, const char *voice, char *err, size_t errlen)
{
    sam_speech *t = calloc(1, sizeof *t);
    char sdf[1024];
    FILE *f;
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (!dir || !*dir) dir = ".";
    if (!voice || !*voice) voice = "Sam";
    sam_params_default(&t->p);
    snprintf(t->spd, sizeof t->spd, "%s/%s.spd", dir, voice);
    snprintf(t->lex, sizeof t->lex, "%s/LTTS1033.LXA", dir);
    snprintf(t->lts, sizeof t->lts, "%s/r1033tts.LXA", dir);
    snprintf(sdf, sizeof sdf, "%s/%s.sdf", dir, voice);
    f = fopen(sdf, "rb"); /* the voice's base pitch ("Vois" header, int at 0x318) */
    if (f) {
        unsigned char b[0x31c];
        if (fread(b, 1, sizeof b, f) == sizeof b && !memcmp(b, "Vois", 4))
            t->p.base_pitch = (float)(b[0x318] | b[0x319] << 8 | b[0x31a] << 16 | (unsigned)b[0x31b] << 24);
        fclose(f);
    }
    t->volume = 100;
    t->fx_tail_ms = -1;
    if (load_engine(t, err, errlen)) {
        free(t);
        return NULL;
    }
    return t;
}

void sam_tts_close(sam_speech *t)
{
    if (!t) return;
    sam_tts_free(t->eng);
    free(t);
}

void sam_tts_set_rate(sam_speech *t, int rate) { t->rate = rate < -10 ? -10 : rate > 18 ? 18 : rate; }
void sam_tts_set_volume(sam_speech *t, int v) { t->volume = v < 0 ? 0 : v > 100 ? 100 : v; }
void sam_tts_set_pitch(sam_speech *t, int p) { t->pitch = p < -10 ? -10 : p > 10 ? 10 : p; }
void sam_tts_cancel(sam_speech *t) { t->cancel = 1; }

/* ClassicVoices patch: a fractional SAPI rate (-10..18, duration divisor 3^(rate/10), the same curve as the
 * integer table) and a pitch shift in semitones (-12..12). Used instead of the integer rate / pitch. */
void sam_tts_set_rate_pitch_f(sam_speech *t, double sapi_rate, double semitones)
{
    t->use_f = 1;
    t->rate_f = sapi_rate < -10.0 ? -10.0 : sapi_rate > 18.0 ? 18.0 : sapi_rate;
    t->pitch_st = semitones < -12.0 ? -12.0 : semitones > 12.0 ? 12.0 : semitones;
}

/* ClassicVoices patch: how long the echo effects ring on after the text (ms, faded out); -1 = upstream */
void sam_tts_set_fx_tail(sam_speech *t, int ms) { t->fx_tail_ms = ms < -1 ? -1 : ms > 5000 ? 5000 : ms; }

/* ClassicVoices patch: replace the voice's base pitch (the .sdf value) - the SAPI 4 RoboSoft voices run the
 * same synthesis at their own pitch. hz <= 0 keeps the current one. Reloads the voice. */
int sam_tts_set_base_pitch(sam_speech *t, double hz)
{
    char err[256];
    if (hz <= 0.0 || (float)hz == t->p.base_pitch) return 0;
    t->p.base_pitch = (float)hz;
    return load_engine(t, err, sizeof err);
}
int sam_tts_sample_rate(const sam_speech *t)
{
    return t && t->p.out_rate > 0 ? t->p.out_rate : SAM_TTS_SAMPLE_RATE;
}

int sam_tts_set_effect(sam_speech *t, const char *name)
{
    int preset = 0, mono = 0, whisper = 0;
    char err[256];
    if (!name || !*name) name = "none";
    if (sam4fx_lookup(name, &preset, &mono, &whisper)) return -1;
    if (preset == t->fx_preset && mono == t->p.monotone && whisper == t->p.whisper) return 0;
    t->fx_preset = preset;
    t->fx_on = preset != 0 || whisper;
    t->p.monotone = mono;
    t->p.whisper = whisper;
    return load_engine(t, err, sizeof err); /* monotone / whisper live in the synthesizer */
}

/* ------------------------------------------------------------------ SAPI XML */

/* A speak call is a list of ops: runs of plain text (each with the state that was in force) and
 * explicit silences. Bookmarks keep their position in the stripped text and are reported when the
 * word at or after them is spoken. */
typedef struct {
    int kind; /* 0 = text run, 1 = silence */
    long start, end;
    int msec;
    int rate, volume, pitch;
} xop;

typedef struct {
    long pos;
    char *name;
} xbm;

typedef struct xdoc {
    char *text; /* the stripped UTF-8 text */
    long n;
    long *bmap; /* [n+1]: stripped byte -> byte in the original text */
    xop *ops;
    int nop, opcap;
    xbm *bm;
    int nbm, bmcap;
} xdoc;

static void xdoc_free(xdoc *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->nbm; i++) free(d->bm[i].name);
    free(d->bm);
    free(d->ops);
    free(d->bmap);
    free(d->text);
    free(d);
}

static int xd_put(xdoc *d, int c, long src)
{
    d->text[d->n] = (char)c;
    d->bmap[d->n] = src;
    d->n++;
    return 0;
}

static int xd_utf8(xdoc *d, unsigned cp, long src)
{
    if (cp < 0x80) return xd_put(d, (int)cp, src);
    if (cp < 0x800) {
        xd_put(d, 0xc0 | (int)(cp >> 6), src);
        return xd_put(d, 0x80 | (int)(cp & 63), src);
    }
    xd_put(d, 0xe0 | (int)(cp >> 12), src);
    xd_put(d, 0x80 | (int)(cp >> 6 & 63), src);
    return xd_put(d, 0x80 | (int)(cp & 63), src);
}

static xop *xd_op(xdoc *d)
{
    if (d->nop == d->opcap) {
        int cap = d->opcap ? d->opcap * 2 : 8;
        xop *o = realloc(d->ops, sizeof *o * (size_t)cap);
        if (!o) return NULL;
        d->ops = o;
        d->opcap = cap;
    }
    memset(&d->ops[d->nop], 0, sizeof *d->ops);
    return &d->ops[d->nop++];
}

static int tag_name_is(const char *s, long n, const char *name)
{
    long i;
    for (i = 0; i < n && name[i]; i++)
        if (tolower((unsigned char)s[i]) != name[i]) return 0;
    return i == n && !name[i];
}

/* value of attribute `name` inside the tag body s[0..n); returns its length, or -1 */
static long tag_attr(const char *s, long n, const char *name, long *vpos)
{
    long i = 0, k = (long)strlen(name);
    while (i < n) {
        long ns, ne;
        while (i < n && (unsigned char)s[i] <= ' ') i++;
        ns = i;
        while (i < n && s[i] != '=' && (unsigned char)s[i] > ' ') i++;
        ne = i;
        while (i < n && (unsigned char)s[i] <= ' ') i++;
        if (i < n && s[i] == '=') {
            long vs, ve;
            char q;
            i++;
            while (i < n && (unsigned char)s[i] <= ' ') i++;
            q = i < n && (s[i] == '"' || s[i] == '\'') ? s[i] : 0;
            if (q) i++;
            vs = i;
            while (i < n && (q ? s[i] != q : (unsigned char)s[i] > ' ')) i++;
            ve = i;
            if (q && i < n) i++;
            if (ne - ns == k) {
                long j;
                int ok = 1;
                for (j = 0; j < k; j++)
                    if (tolower((unsigned char)s[ns + j]) != name[j]) ok = 0;
                if (ok) {
                    *vpos = vs;
                    return ve - vs;
                }
            }
        }
    }
    return -1;
}

typedef struct {
    int rate, volume, pitch;
} xstate;

/* Turns SAPI XML (or, with xml = 0, plain text) into a run list. Returns NULL on failure. */
static xdoc *xml_prepare(const char *s, int xml, int rate, int volume, int pitch)
{
    size_t len = strlen(s);
    xdoc *d = calloc(1, sizeof *d);
    xstate st, stack[32];
    int depth = 0;
    long i = 0, run_start = 0;
    if (!d) return NULL;
    d->text = malloc(len + 1);
    d->bmap = malloc(sizeof(long) * (len + 2));
    if (!d->text || !d->bmap) {
        xdoc_free(d);
        return NULL;
    }
    st.rate = rate;
    st.volume = volume;
    st.pitch = pitch;
    if (!xml) {
        memcpy(d->text, s, len);
        d->n = (long)len;
        for (i = 0; i <= (long)len; i++) d->bmap[i] = i;
        d->text[len] = 0;
        { /* one run, the whole text */
            xop *o = xd_op(d);
            if (!o) {
                xdoc_free(d);
                return NULL;
            }
            o->kind = 0;
            o->start = 0;
            o->end = d->n;
            o->rate = rate;
            o->volume = volume;
            o->pitch = pitch;
        }
        return d;
    }
    while (i < (long)len) {
        if (s[i] == '<') {
            long ts, te, ns, ne;
            int closing = 0, selfclose = 0, changed = 0;
            xstate ns_st = st;
            if (!strncmp(s + i, "<!--", 4)) { /* a comment vanishes */
                const char *e = strstr(s + i + 4, "-->");
                i = e ? (long)(e - s) + 3 : (long)len;
                continue;
            }
            ts = i + 1;
            te = ts;
            while (te < (long)len && s[te] != '>') te++;
            if (te >= (long)len) { /* an unterminated '<': plain text */
                xd_put(d, s[i], i);
                i++;
                continue;
            }
            if (ts < te && s[ts] == '/') {
                closing = 1;
                ts++;
            }
            if (te > ts && s[te - 1] == '/') selfclose = 1;
            ns = ts;
            ne = ns;
            while (ne < te && (unsigned char)s[ne] > ' ' && s[ne] != '/') ne++;
            if (tag_name_is(s + ns, ne - ns, "bookmark") && !closing) {
                long vp, vl = tag_attr(s + ne, te - ne - (selfclose ? 1 : 0), "mark", &vp);
                if (vl >= 0) {
                    if (d->nbm == d->bmcap) {
                        int cap = d->bmcap ? d->bmcap * 2 : 8;
                        xbm *b = realloc(d->bm, sizeof *b * (size_t)cap);
                        if (!b) {
                            xdoc_free(d);
                            return NULL;
                        }
                        d->bm = b;
                        d->bmcap = cap;
                    }
                    d->bm[d->nbm].pos = d->n;
                    d->bm[d->nbm].name = malloc((size_t)vl + 1);
                    if (!d->bm[d->nbm].name) {
                        xdoc_free(d);
                        return NULL;
                    }
                    memcpy(d->bm[d->nbm].name, s + ne + vp, (size_t)vl);
                    d->bm[d->nbm].name[vl] = 0;
                    d->nbm++;
                }
            } else if (tag_name_is(s + ns, ne - ns, "silence") && !closing) {
                long vp, vl = tag_attr(s + ne, te - ne - (selfclose ? 1 : 0), "msec", &vp);
                if (vl > 0) {
                    xop *o;
                    if (d->n > run_start) { /* close the run before the pause */
                        o = xd_op(d);
                        if (!o) {
                            xdoc_free(d);
                            return NULL;
                        }
                        o->kind = 0;
                        o->start = run_start;
                        o->end = d->n;
                        o->rate = st.rate;
                        o->volume = st.volume;
                        o->pitch = st.pitch;
                    }
                    run_start = d->n;
                    o = xd_op(d);
                    if (!o) {
                        xdoc_free(d);
                        return NULL;
                    }
                    o->kind = 1;
                    o->msec = (int)strtol(s + ne + vp, NULL, 10);
                    o->start = o->end = d->n;
                }
            } else if (tag_name_is(s + ns, ne - ns, "rate") || tag_name_is(s + ns, ne - ns, "volume") ||
                       tag_name_is(s + ns, ne - ns, "pitch")) {
                if (closing) {
                    if (depth > 0) ns_st = stack[--depth];
                } else {
                    long vp, vl;
                    long body = te - ne - (selfclose ? 1 : 0);
                    if (tag_name_is(s + ns, ne - ns, "rate")) {
                        if ((vl = tag_attr(s + ne, body, "absspeed", &vp)) >= 0)
                            ns_st.rate = (int)strtol(s + ne + vp, NULL, 10);
                        else if ((vl = tag_attr(s + ne, body, "speed", &vp)) >= 0)
                            ns_st.rate = st.rate + (int)strtol(s + ne + vp, NULL, 10);
                    } else if (tag_name_is(s + ns, ne - ns, "volume")) {
                        if ((vl = tag_attr(s + ne, body, "level", &vp)) >= 0)
                            ns_st.volume = (int)strtol(s + ne + vp, NULL, 10);
                    } else {
                        if ((vl = tag_attr(s + ne, body, "absmiddle", &vp)) >= 0)
                            ns_st.pitch = (int)strtol(s + ne + vp, NULL, 10);
                        else if ((vl = tag_attr(s + ne, body, "middle", &vp)) >= 0)
                            ns_st.pitch = st.pitch + (int)strtol(s + ne + vp, NULL, 10);
                    }
                    if (!selfclose && depth < 32) stack[depth++] = st;
                    (void)vl;
                }
                changed = ns_st.rate != st.rate || ns_st.volume != st.volume || ns_st.pitch != st.pitch;
            }
            if (changed) {
                if (d->n > run_start) {
                    xop *o = xd_op(d);
                    if (!o) {
                        xdoc_free(d);
                        return NULL;
                    }
                    o->kind = 0;
                    o->start = run_start;
                    o->end = d->n;
                    o->rate = st.rate;
                    o->volume = st.volume;
                    o->pitch = st.pitch;
                }
                run_start = d->n;
                st = ns_st;
            }
            i = te + 1;
            continue;
        }
        if (s[i] == '&') { /* entities */
            long j = i + 1;
            while (j < (long)len && s[j] != ';' && j - i < 12) j++;
            if (j < (long)len && s[j] == ';') {
                long n = j - i - 1;
                const char *e = s + i + 1;
                unsigned cp = 0;
                int ok = 1;
                if (n == 3 && !strncmp(e, "amp", 3)) cp = '&';
                else if (n == 2 && !strncmp(e, "lt", 2)) cp = '<';
                else if (n == 2 && !strncmp(e, "gt", 2)) cp = '>';
                else if (n == 4 && !strncmp(e, "quot", 4)) cp = '"';
                else if (n == 4 && !strncmp(e, "apos", 4)) cp = '\'';
                else if (n >= 2 && e[0] == '#') cp = (unsigned)strtoul(e + (e[1] == 'x' || e[1] == 'X' ? 2 : 1), NULL,
                                                                      e[1] == 'x' || e[1] == 'X' ? 16 : 10);
                else ok = 0;
                if (ok && cp) {
                    xd_utf8(d, cp, i);
                    i = j + 1;
                    continue;
                }
            }
        }
        xd_put(d, s[i], i);
        i++;
    }
    d->text[d->n] = 0;
    d->bmap[d->n] = (long)len;
    if (d->n > run_start || d->nop == 0) {
        xop *o = xd_op(d);
        if (!o) {
            xdoc_free(d);
            return NULL;
        }
        o->kind = 0;
        o->start = run_start;
        o->end = d->n;
        o->rate = st.rate;
        o->volume = st.volume;
        o->pitch = st.pitch;
    }
    return d;
}

/* ------------------------------------------------------------------ events */

/* the front end reports UTF-16 unit positions (sam_norm_new decodes UTF-8 that way); map them back
 * to byte offsets of the run */
static long *utf16_to_utf8_map(const char *s, long n, long *nunits)
{
    long i = 0, k = 0;
    long *m = malloc(sizeof(long) * (size_t)(n + 2));
    if (!m) return NULL;
    while (i < n) {
        unsigned c = (unsigned char)s[i];
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0, ok = 0, j;
        if (extra) {
            ok = 1;
            for (j = 1; j <= extra; j++) {
                unsigned dd = i + j < n ? (unsigned char)s[i + j] : 0;
                if ((dd & 0xc0) != 0x80) ok = 0;
            }
        }
        m[k++] = i;
        i += ok ? extra + 1 : 1; /* sam_norm_new folds astral planes to one U+FFFD unit */
    }
    m[k] = n;
    *nunits = k;
    return m;
}

static long map_unit(const sam_speech *t, long u) { return u < 0 ? 0 : u > t->nmap ? t->map[t->nmap] : t->map[u]; }

/* run byte offset -> byte offset in the text the caller gave us */
static long to_src(const sam_speech *t, long run_byte)
{
    long p = t->run_start + run_byte;
    return t->bmap ? t->bmap[p] : p;
}

static void emit(sam_speech *t, int type, uint64_t pos, long run_pos, long run_len, const char *name)
{
    sam_event e;
    if (!t->cb->event) return;
    e.type = type;
    e.audio_pos = pos;
    e.text_pos = to_src(t, run_pos);
    e.text_len = to_src(t, run_pos + run_len) - e.text_pos;
    e.name = name;
    t->cb->event(&e, t->cb->user);
}

/* bookmarks up to a position in the stripped text (-1 = all that are left in this run) */
static void emit_bookmarks(sam_speech *t, uint64_t pos, long upto)
{
    for (; t->next_bm < t->doc->nbm; t->next_bm++) {
        const xbm *b = &t->doc->bm[t->next_bm];
        sam_event e;
        if (upto >= 0 && b->pos > upto) break;
        if (!t->cb->event) continue;
        e.type = SAM_EV_BOOKMARK;
        e.audio_pos = pos;
        e.text_pos = t->bmap ? t->bmap[b->pos] : b->pos;
        e.text_len = 0;
        e.name = b->name;
        t->cb->event(&e, t->cb->user);
    }
}

static void on_mark(void *user, const sam_mark *m)
{
    sam_speech *t = user;
    uint64_t at = t->run_base + (uint64_t)m->audio_pos;
    if (m->flags & 2) emit(t, SAM_EV_SENTENCE, at, map_unit(t, m->sent_pos), map_unit(t, m->sent_pos + m->sent_len) - map_unit(t, m->sent_pos), NULL);
    if (m->flags & 1) {
        long p = map_unit(t, m->word_pos), l = map_unit(t, m->word_pos + m->word_len) - p;
        emit_bookmarks(t, at, t->run_start + p);
        if (p != t->last_word && l > 0) emit(t, SAM_EV_WORD, at, p, l, NULL);
        t->last_word = p;
    }
}

/* ------------------------------------------------------------------ audio */

static void deliver(sam_speech *t, int16_t *pcm, size_t n, int volume)
{
    size_t i;
    if (t->stopped) return;
    if (t->fx) sam4fx_process_s16(t->fx, pcm, (int)n); /* one call per chunk, like sam_say */
    if (volume != 100)
        for (i = 0; i < n; i++) pcm[i] = (int16_t)((int)pcm[i] * volume / 100);
    if (t->cb->audio && t->cb->audio(pcm, n, t->cb->user)) {
        t->stopped = 1;
        t->cancel = 1;
    }
    t->base += n;
}

typedef struct {
    sam_speech *t;
    int volume;
} pcm_ctx;

static void silence(sam_speech *t, size_t n, int volume);

static void on_pcm(const int16_t *pcm, size_t n, void *user)
{
    pcm_ctx *c = user;
    int16_t *b;
    if (c->t->stopped || !n) return;
    if (c->t->fx_tail_ms >= 0) {
        /* ClassicVoices patch: the engine's pauses are runs of exact zeros. Hold the trailing ones back
         * and deliver them only when more sound follows, so pauses between sentences keep their echo but
         * the ~300 ms after the last word never reaches the effect (sam_tts_speak drops it). */
        size_t last = n;
        while (last > 0 && pcm[last - 1] == 0) last--;
        if (last == 0) {
            c->t->held_zeros += n;
            return;
        }
        if (c->t->held_zeros) {
            size_t z = c->t->held_zeros;
            c->t->held_zeros = 0;
            silence(c->t, z, c->volume);
            if (c->t->stopped) return;
        }
        c->t->held_zeros = n - last;
        n = last;
    }
    b = malloc(n * sizeof *b);
    if (!b) return;
    memcpy(b, pcm, n * sizeof *b);
    deliver(c->t, b, n, c->volume);
    free(b);
}

static void silence(sam_speech *t, size_t n, int volume)
{
    int16_t *b;
    if (!n || t->stopped) return;
    b = calloc(n, sizeof *b);
    if (!b) return;
    deliver(t, b, n, volume);
    free(b);
}

/* ------------------------------------------------------------------ speak */

/* ClassicVoices patch (singing mode): sing plain text (an automatic melody) or, literal != 0, a score, through
 * the same effect / volume / pause handling as sam_tts_speak. The rate scales the tempo, the pitch transposes. */
int sam_tts_sing_speech(sam_speech *t, const char *utf8, int literal, const sam_sing_cfg *cfg, const sam_callbacks *cb,
                        int *mismatches)
{
    sam_sing_opts so;
    pcm_ctx pc;
    double r, st;
    int rc;
    if (mismatches) *mismatches = 0;
    if (!t || !utf8 || !cb) return -1;
    t->cancel = 0;
    t->stopped = 0;
    t->base = 0;
    t->cb = cb;
    t->orig = utf8;
    t->held_zeros = 0;
    t->bmap = NULL;
    t->doc = NULL;
    if (t->fx_on) {
        t->fx = sam4fx_new(t->fx_preset, sam_tts_sample_rate(t));
        if (t->fx) {
            sam4fx_set_input_gain(t->fx, (float)pow(10.0, sam4fx_default_trim_db(t->fx_preset) / 20.0));
            if (t->p.whisper) {
                static const float taps[3] = {0.25f, -0.5f, 0.25f};
                sam4fx_set_fir(t->fx, taps, 3);
            }
        }
    }
    r = t->use_f ? t->rate_f : (double)t->rate;
    st = t->use_f ? t->pitch_st : t->pitch * 0.5;
    r = r < -10.0 ? -10.0 : r > 18.0 ? 18.0 : r;
    memset(&so, 0, sizeof so);
    so.cancel = &t->cancel;
    so.tempo_scale = pow(3.0, r / 10.0);
    so.transpose = st + (cfg ? cfg->transpose : 0.0);
    so.vibrato_cents = cfg ? cfg->vibrato_cents : 0.0f;
    so.vibrato_rate = cfg && cfg->vibrato_rate > 0.0f ? cfg->vibrato_rate : 5.5f;
    pc.t = t;
    pc.volume = t->volume < 0 ? 0 : t->volume > 100 ? 100 : t->volume;
    if (literal) {
        /* a literal score's tempo line stays as written; the speaking rate does not apply */
        rc = sam_tts_sing_ex(t->eng, utf8, &so, on_pcm, &pc);
        if (mismatches) *mismatches = so.mismatches;
    } else {
        rc = sam_tts_sing_text(t->eng, utf8, &so, on_pcm, &pc);
    }
    t->held_zeros = 0;
    if (t->fx) {
        int k;
        if (t->fx_tail_ms < 0) {
            for (k = 0; k < 2 && !t->stopped; k++) silence(t, (size_t)(sam_tts_sample_rate(t) / 2), 100);
        } else {
            int steps = t->fx_tail_ms / 10;
            for (k = 0; k < steps && !t->stopped; k++)
                silence(t, (size_t)(sam_tts_sample_rate(t) / 100), 100 - 100 * (k + 1) / (steps + 1));
        }
        sam4fx_free(t->fx);
        t->fx = NULL;
    }
    if (rc >= 0 && (t->cancel || t->stopped)) rc = 1;
    return rc;
}

int sam_tts_speak(sam_speech *t, const char *utf8, int flags, const sam_callbacks *cb)
{
    xdoc *d;
    int i, rc = 0;
    char *run = NULL;
    long runcap = 0;
    if (!t || !utf8 || !cb) return -1;
    t->cancel = 0;
    t->stopped = 0;
    t->base = 0;
    t->cb = cb;
    t->orig = utf8;
    t->next_bm = 0;
    t->held_zeros = 0;
    d = xml_prepare(utf8, (flags & SAM_SPEAK_XML) != 0, t->rate, t->volume, t->pitch);
    if (!d) return -1;
    t->doc = d;
    t->bmap = d->bmap;
    if (t->fx_on) {
        t->fx = sam4fx_new(t->fx_preset, sam_tts_sample_rate(t));
        if (t->fx) {
            sam4fx_set_input_gain(t->fx, (float)pow(10.0, sam4fx_default_trim_db(t->fx_preset) / 20.0));
            if (t->p.whisper) {
                static const float taps[3] = {0.25f, -0.5f, 0.25f};
                sam4fx_set_fir(t->fx, taps, 3);
            }
        }
    }
    for (i = 0; i < d->nop && rc == 0 && !t->cancel; i++) {
        const xop *o = &d->ops[i];
        sam_speak_opts so;
        pcm_ctx pc;
        long n = o->end - o->start;
        if (o->kind == 1) { /* <silence msec="..."/> */
            if (t->held_zeros) { /* ClassicVoices patch: keep the held pause in order */
                size_t z = t->held_zeros;
                t->held_zeros = 0;
                silence(t, z, 100);
            }
            silence(t, (size_t)((long long)o->msec * sam_tts_sample_rate(t) / 1000), o->volume);
            continue;
        }
        if (n <= 0) continue;
        if (n + 1 > runcap) {
            char *b = realloc(run, (size_t)n + 1);
            if (!b) {
                rc = -1;
                break;
            }
            run = b;
            runcap = n + 1;
        }
        memcpy(run, d->text + o->start, (size_t)n);
        run[n] = 0;
        free((void *)t->map);
        t->map = utf16_to_utf8_map(run, n, &t->nmap);
        if (!t->map) {
            rc = -1;
            break;
        }
        t->run_start = o->start;
        t->run_base = t->base;
        t->last_word = -1;
        memset(&so, 0, sizeof so);
        so.sapi_rate = rate_factor(o->rate < -10 ? -10 : o->rate > 18 ? 18 : o->rate);
        so.pitch_offset = (float)((o->pitch < -10 ? -10 : o->pitch > 10 ? 10 : o->pitch) * (5.0 / 12.0) / 10.0);
        if (t->use_f) { /* ClassicVoices patch: continuous values, plus any XML offsets on top */
            double r = t->rate_f + (o->rate - t->rate);
            r = r < -10.0 ? -10.0 : r > 18.0 ? 18.0 : r;
            so.sapi_rate = pow(3.0, r / 10.0);
            so.pitch_offset = (float)(t->pitch_st / 12.0 + (o->pitch - t->pitch) * (5.0 / 12.0) / 10.0);
        }
        so.cancel = &t->cancel;
        so.mark_cb = on_mark;
        so.user = t;
        pc.t = t;
        pc.volume = o->volume < 0 ? 0 : o->volume > 100 ? 100 : o->volume;
        rc = sam_tts_speak_ex(t->eng, run, &so, on_pcm, &pc);
        if (rc == 0 && !t->cancel) emit_bookmarks(t, t->base, o->end); /* bookmarks after the last word */
    }
    t->held_zeros = 0; /* ClassicVoices patch: the silence after the last word is dropped */
    if (t->fx) { /* let the echoes die away, exactly as sam_say does */
        int k;
        if (t->fx_tail_ms < 0) {
            for (k = 0; k < 2 && !t->stopped; k++) silence(t, (size_t)(sam_tts_sample_rate(t) / 2), 100);
        } else { /* ClassicVoices patch: a shorter tail in 10 ms steps, fading linearly to nothing */
            int steps = t->fx_tail_ms / 10;
            for (k = 0; k < steps && !t->stopped; k++)
                silence(t, (size_t)(sam_tts_sample_rate(t) / 100), 100 - 100 * (k + 1) / (steps + 1));
        }
        sam4fx_free(t->fx);
        t->fx = NULL;
    }
    if (rc >= 0 && (t->cancel || t->stopped)) rc = 1;
    if (rc == 0) {
        emit_bookmarks(t, t->base, -1);
        if (cb->event) {
            sam_event e;
            memset(&e, 0, sizeof e);
            e.type = SAM_EV_END;
            e.audio_pos = t->base;
            e.text_pos = (long)strlen(utf8);
            cb->event(&e, cb->user);
        }
    }
    free(run);
    free((void *)t->map);
    t->map = NULL;
    t->doc = NULL;
    t->bmap = NULL;
    xdoc_free(d);
    return rc;
}
