/* ClassicVoices bridge - see cv_bridge.h. */
#include "cv_bridge.h"
#include "sam_tts.h"
#include "sam.h" /* sam_sing_is_score */

#include <stdlib.h>
#include <string.h>

#include "cv_fold_table.h"

/* ================================================================== text sanitizer */

typedef struct {
    char *b;
    size_t n, cap;
    int oom;
} sbuf;

static void sb_putc(sbuf *s, char c)
{
    if (s->oom) return;
    if (s->n + 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        char *b = realloc(s->b, cap);
        if (!b) {
            s->oom = 1;
            return;
        }
        s->b = b;
        s->cap = cap;
    }
    s->b[s->n++] = c;
    s->b[s->n] = 0;
}

static void sb_puts(sbuf *s, const char *str)
{
    while (*str) sb_putc(s, *str++);
}

static void sb_utf8(sbuf *s, unsigned cp)
{
    if (cp < 0x80) {
        sb_putc(s, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(s, (char)(0xc0 | (cp >> 6)));
        sb_putc(s, (char)(0x80 | (cp & 63)));
    } else {
        sb_putc(s, (char)(0xe0 | (cp >> 12)));
        sb_putc(s, (char)(0x80 | ((cp >> 6) & 63)));
        sb_putc(s, (char)(0x80 | (cp & 63)));
    }
}

/* A spoken word for a symbol the 2001 normalizer silently drops (it says "a b" for "a → b"). Kept short
 * on purpose: only symbols a screen reader meets often. Sorted by code point. */
typedef struct {
    unsigned cp;
    const char *say;
} cv_word;

static const cv_word CV_WORDS[] = {
    {0x00A7, " section "},
    {0x00B5, " micro "},
    {0x00B6, " paragraph "},
    {0x00D7, " times "},
    {0x03BC, " micro "},
    {0x03C0, " pi "},
    {0x2103, " degrees Celsius "},
    {0x2109, " degrees Fahrenheit "},
    {0x2116, " number "},
    {0x2153, " 1/3 "},
    {0x2154, " 2/3 "},
    {0x2155, " 1/5 "},
    {0x2156, " 2/5 "},
    {0x2157, " 3/5 "},
    {0x2158, " 4/5 "},
    {0x2159, " 1/6 "},
    {0x215A, " 5/6 "},
    {0x215B, " 1/8 "},
    {0x215C, " 3/8 "},
    {0x215D, " 5/8 "},
    {0x215E, " 7/8 "},
    {0x2190, " left arrow "},
    {0x2191, " up arrow "},
    {0x2192, " right arrow "},
    {0x2193, " down arrow "},
    {0x2194, " left right arrow "},
    {0x21E5, " tab "},
    {0x21E7, " shift "},
    {0x221E, " infinity "},
    {0x2248, " approximately "},
    {0x2260, " not equal to "},
    {0x2264, " less than or equal to "},
    {0x2265, " greater than or equal to "},
    {0x2303, " control "},
    {0x2318, " command "},
    {0x2325, " option "},
    {0x232B, " delete "},
    {0x238B, " escape "},
    {0x23CE, " return "},
    {0x2713, " check mark "},
    {0x2714, " check mark "},
};

/* Characters the engine's normalizer already verbalizes (Latin-1 / Windows-1252 symbols): passed as-is. */
static int engine_knows(unsigned cp)
{
    switch (cp) {
    case 0x00A2: case 0x00A3: case 0x00A5: case 0x00A6: case 0x00A9: case 0x00AE: case 0x00B0: case 0x00B1:
    case 0x00B2: case 0x00B3: case 0x00B9: case 0x00BC: case 0x00BD: case 0x00BE: case 0x00F7: case 0x20AC:
    case 0x2122: case 0x2030:
        return 1;
    default:
        return 0;
    }
}

static const char *fold_lookup(unsigned cp)
{
    size_t lo = 0, hi = sizeof CV_FOLD / sizeof *CV_FOLD;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (CV_FOLD[mid].cp == cp) return CV_FOLD[mid].ascii;
        if (CV_FOLD[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static const char *word_lookup(unsigned cp)
{
    size_t lo = 0, hi = sizeof CV_WORDS / sizeof *CV_WORDS;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (CV_WORDS[mid].cp == cp) return CV_WORDS[mid].say;
        if (CV_WORDS[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* One code point -> speakable text. Anything the engine could not say becomes a space (so the words
 * around it stay separate) or disappears when it is invisible formatting. */
static void map_cp(sbuf *s, unsigned cp)
{
    const char *w;
    if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0; /* fullwidth ASCII */
    if (cp < 0x20 || cp == 0x7F) {
        if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f') sb_putc(s, ' ');
        return;
    }
    if (cp < 0x7F) {
        sb_putc(s, (char)cp);
        return;
    }
    if (engine_knows(cp)) {
        sb_utf8(s, cp);
        return;
    }
    if ((w = word_lookup(cp)) != NULL) {
        sb_puts(s, w);
        return;
    }
    if ((w = fold_lookup(cp)) != NULL) {
        sb_puts(s, w);
        return;
    }
    switch (cp) {
    /* invisible: drop without a space (soft hyphen, zero-width marks, joiners, direction marks, BOM) */
    case 0x00AD: case 0x034F: case 0x061C: case 0x180E: case 0x200B: case 0x200C: case 0x200D: case 0x200E:
    case 0x200F: case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E: case 0x2060: case 0x2061:
    case 0x2062: case 0x2063: case 0x2064: case 0x2066: case 0x2067: case 0x2068: case 0x2069: case 0xFEFF:
        return;
    /* apostrophes and primes */
    case 0x00B4: case 0x02B9: case 0x02BB: case 0x02BC: case 0x02BD: case 0x2018: case 0x2019: case 0x201A:
    case 0x201B: case 0x2032: case 0x2035:
        sb_putc(s, '\'');
        return;
    /* double quotes */
    case 0x00AB: case 0x00BB: case 0x02BA: case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
    case 0x2036: case 0x301D: case 0x301E:
        sb_putc(s, '"');
        return;
    /* hyphens, en dash, minus */
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2043: case 0x2212: case 0xFE63:
        sb_putc(s, '-');
        return;
    /* em dash, horizontal bar: a spoken break, not a hyphenated word */
    case 0x2014: case 0x2015: case 0x2E3A: case 0x2E3B:
        sb_puts(s, " - ");
        return;
    case 0x2024: sb_putc(s, '.'); return;
    case 0x2025: sb_puts(s, ".."); return;
    case 0x2026: sb_puts(s, "..."); return;
    case 0x203C: sb_puts(s, "!"); return;
    case 0x2047: sb_puts(s, "?"); return;
    case 0x2048: case 0x2049: sb_puts(s, "?!"); return;
    case 0x2044: case 0x2215: sb_putc(s, '/'); return;
    case 0x2217: sb_putc(s, '*'); return;
    case 0x3001: case 0xFF64: sb_puts(s, ", "); return;
    case 0x3002: case 0xFF61: sb_puts(s, ". "); return;
    default:
        break;
    }
    if (cp >= 0x0300 && cp <= 0x036F) return;                  /* combining accents: keep the base letter */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return;                  /* variation selectors */
    if (cp >= 0xE0000 && cp <= 0xE007F) return;                /* tag characters */
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return;                /* skin-tone modifiers */
    sb_putc(s, ' ');                                           /* emoji, CJK, other scripts, symbols */
}

/* decode one UTF-8 sequence; invalid bytes are skipped (returns 0 with *adv = 1) */
static unsigned next_cp(const unsigned char *p, size_t left, size_t *adv)
{
    unsigned c = p[0], cp;
    int extra, i;
    *adv = 1;
    if (c < 0x80) return c;
    if (c >= 0xc2 && c < 0xe0) {
        extra = 1;
        cp = c & 0x1f;
    } else if (c >= 0xe0 && c < 0xf0) {
        extra = 2;
        cp = c & 0x0f;
    } else if (c >= 0xf0 && c < 0xf5) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if ((size_t)extra >= left) return 0;
    for (i = 1; i <= extra; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *adv = (size_t)extra + 1;
    return cp;
}

char *cv_sanitize_alloc(const char *utf8)
{
    sbuf raw = {0}, out = {0};
    const unsigned char *p = (const unsigned char *)utf8;
    size_t len, i = 0, k, letters = 0;
    int space = 1;
    if (!utf8) return NULL;
    len = strlen(utf8);
    sb_putc(&raw, ' ');
    while (i < len) {
        size_t adv;
        unsigned cp = next_cp(p + i, len - i, &adv);
        if (cp) map_cp(&raw, cp);
        i += adv;
    }
    if (raw.oom) {
        free(raw.b);
        return NULL;
    }
    /* collapse whitespace, trim, and break letter runs the letter-to-sound model cannot take (its limit
     * is 127 letters; past that the original engine fails the whole call) */
    sb_putc(&out, 0);
    out.n = 0;
    for (k = 0; k < raw.n; k++) {
        char c = raw.b[k];
        int is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (c == ' ') {
            if (!space) sb_putc(&out, ' ');
            space = 1;
            letters = 0;
            continue;
        }
        if (is_letter) {
            if (letters == 40) {
                sb_putc(&out, ' ');
                letters = 0;
            }
            letters++;
        } else {
            letters = 0;
        }
        sb_putc(&out, c);
        space = 0;
    }
    free(raw.b);
    if (out.oom) {
        free(out.b);
        return NULL;
    }
    while (out.n > 0 && out.b[out.n - 1] == ' ') out.b[--out.n] = 0;
    return out.b;
}

void cv_free(void *p) { free(p); }

/* ================================================================== voice */

struct cv_voice {
    sam_speech *s;
};

/* How long each effect may ring on after the last word, faded out (the engine's own ~300 ms of silence
 * after the last word is dropped first, so this is all the tail there is). Long enough for the effect to
 * be heard - Space's 400 ms repeat of the last syllable, Stadium's slaps - short enough that VoiceOver
 * is not kept waiting. */
static int fx_tail_ms(const char *effect)
{
    if (!effect || !*effect || !strcmp(effect, "none") || !strcmp(effect, "monotone") || !strcmp(effect, "whisper"))
        return 0;
    if (!strcmp(effect, "space")) return 800;
    if (!strcmp(effect, "stadium")) return 600;
    if (!strcmp(effect, "hall") || !strcmp(effect, "room")) return 350;
    return 200; /* the RoboSofts' 10 ms ringing loop */
}

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (!err || !errlen) return;
    strncpy(err, msg, errlen - 1);
    err[errlen - 1] = 0;
}

cv_voice *cv_voice_open(const char *dir, const char *spd_name, const char *effect, double base_pitch_hz, char *err,
                        size_t errlen)
{
    cv_voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->s = sam_tts_open(dir, spd_name, err, errlen);
    if (!v->s) {
        free(v);
        return NULL;
    }
    if (effect && *effect && strcmp(effect, "none") != 0 && sam_tts_set_effect(v->s, effect) != 0) {
        set_err(err, errlen, "unknown effect");
        sam_tts_close(v->s);
        free(v);
        return NULL;
    }
    sam_tts_set_fx_tail(v->s, fx_tail_ms(effect));
    if (base_pitch_hz > 0.0 && sam_tts_set_base_pitch(v->s, base_pitch_hz) != 0) {
        set_err(err, errlen, "cannot reload voice");
        sam_tts_close(v->s);
        free(v);
        return NULL;
    }
    return v;
}

void cv_voice_close(cv_voice *v)
{
    if (!v) return;
    sam_tts_close(v->s);
    free(v);
}

int cv_voice_sample_rate(const cv_voice *v) { return v ? sam_tts_sample_rate(v->s) : SAM_TTS_SAMPLE_RATE; }

/* ================================================================== streaming silence trim
 *
 * The engine pads every call with silence (about 300 ms at the end), and the echo effects run a further
 * second of silence through the reverb, whose faint ringing (the RoboSofts' 94% feedback loop) lasts
 * most of it. VoiceOver speaks in short pieces - one per digit in a phone number - so that padding
 * would stack into dead air.
 *
 * Start: samples at or below START_FLOOR before the first sound are dropped (keeping PRE samples).
 * Inside: samples at or below END_FLOOR are held back and released as soon as sound resumes, so pauses
 * between words and sentences stay exactly as the engine made them.
 * End: whatever is held after the last sound above END_FLOOR (about -42 dBFS) is dropped, keeping TAIL
 * samples faded out so the cut cannot click. */

enum { START_FLOOR = 40, END_FLOOR = 250, PRE = 110 /* 5 ms */, TAIL = 662 /* 30 ms */ };

typedef struct {
    cv_pcm_fn fn;
    void *user;
    int trim, started, stopped;
    int16_t pre[PRE];
    size_t npre, pre_pos;
    int16_t *quiet;
    size_t nq, qcap;
    int16_t *out;
    size_t ocap;
} trimmer;

static int tr_reserve(int16_t **b, size_t *cap, size_t need)
{
    if (need <= *cap) return 0;
    {
        size_t c = *cap ? *cap : 4096;
        int16_t *nb;
        while (c < need) c *= 2;
        nb = realloc(*b, c * sizeof *nb);
        if (!nb) return -1;
        *b = nb;
        *cap = c;
    }
    return 0;
}

static int tr_emit(trimmer *t, const int16_t *pcm, size_t n)
{
    if (!n || t->stopped) return t->stopped;
    if (t->fn(pcm, n, t->user)) t->stopped = 1;
    return t->stopped;
}

static int on_audio(const int16_t *pcm, size_t n, void *user)
{
    trimmer *t = user;
    size_t i, no = 0;
    if (t->stopped) return 1;
    if (!t->trim) return tr_emit(t, pcm, n);
    /* worst case this chunk releases everything held plus itself */
    if (tr_reserve(&t->out, &t->ocap, t->nq + PRE + n)) return tr_emit(t, pcm, n);
    for (i = 0; i < n; i++) {
        int16_t s = pcm[i];
        int loud;
        if (!t->started) {
            if (s <= START_FLOOR && s >= -START_FLOOR) { /* ring of the last PRE quiet samples */
                t->pre[t->pre_pos] = s;
                t->pre_pos = (t->pre_pos + 1) % PRE;
                if (t->npre < PRE) t->npre++;
                continue;
            }
            {
                size_t k, start = (t->pre_pos + PRE - t->npre) % PRE;
                for (k = 0; k < t->npre; k++) t->out[no++] = t->pre[(start + k) % PRE];
            }
            t->started = 1;
            t->out[no++] = s;
            continue;
        }
        loud = s > END_FLOOR || s < -END_FLOOR;
        if (!loud) {
            if (tr_reserve(&t->quiet, &t->qcap, t->nq + 1)) return 1;
            t->quiet[t->nq++] = s;
            continue;
        }
        if (t->nq) {
            if (tr_reserve(&t->out, &t->ocap, no + t->nq + (n - i))) return 1;
            memcpy(t->out + no, t->quiet, t->nq * sizeof *t->out);
            no += t->nq;
            t->nq = 0;
        }
        t->out[no++] = s;
    }
    return tr_emit(t, t->out, no);
}

/* the tail: TAIL samples at most, faded out */
static void tr_finish(trimmer *t)
{
    if (t->trim && t->started && !t->stopped && t->nq) {
        size_t k, keep = t->nq < TAIL ? t->nq : TAIL;
        for (k = 0; k < keep; k++) t->quiet[k] = (int16_t)((long)t->quiet[k] * (long)(keep - k) / (long)keep);
        tr_emit(t, t->quiet, keep);
    }
}

struct cv_trimmer {
    trimmer t;
};

cv_trimmer *cv_trimmer_new(int trim, cv_pcm_fn fn, void *user)
{
    cv_trimmer *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->t.fn = fn;
    x->t.user = user;
    x->t.trim = trim;
    return x;
}

int cv_trimmer_feed(cv_trimmer *x, const int16_t *pcm, size_t n) { return on_audio(pcm, n, &x->t); }

int cv_trimmer_finish(cv_trimmer *x)
{
    tr_finish(&x->t);
    return x->t.stopped;
}

void cv_trimmer_free(cv_trimmer *x)
{
    if (!x) return;
    free(x->t.quiet);
    free(x->t.out);
    free(x);
}

int cv_voice_speak(cv_voice *v, const char *utf8, double sapi_rate, double semitones, int trim, cv_pcm_fn fn,
                   void *user)
{
    trimmer t;
    sam_callbacks cb;
    char *clean;
    int rc;
    if (!v || !utf8 || !fn) return -1;
    clean = cv_sanitize_alloc(utf8);
    if (!clean) return -1;
    if (!*clean) { /* nothing speakable: no audio, and never any stand-in text */
        free(clean);
        return 0;
    }
    memset(&t, 0, sizeof t);
    t.fn = fn;
    t.user = user;
    t.trim = trim;
    sam_tts_set_rate_pitch_f(v->s, sapi_rate, semitones);
    cb.audio = on_audio;
    cb.event = NULL;
    cb.user = &t;
    rc = sam_tts_speak(v->s, clean, 0, &cb);
    tr_finish(&t);
    if (rc == 0 && t.stopped) rc = 1;
    free(t.quiet);
    free(t.out);
    free(clean);
    return rc;
}

/* a score token that continues a line: a note ("C4", "F#3", "Bb5") or a number (beats, a MIDI note, a tempo) */
static int score_cont(const char *t, size_t n)
{
    size_t i = 0;
    int digits = 0;
    if (n && ((t[0] >= 'A' && t[0] <= 'G') || (t[0] >= 'a' && t[0] <= 'g'))) {
        i = 1;
        if (i < n && (t[i] == '#' || t[i] == 'b')) i++;
        if (i < n && t[i] == '-') i++;
        for (; i < n && t[i] >= '0' && t[i] <= '9'; i++) digits++;
        return digits && i == n;
    }
    for (; i < n; i++) {
        if (t[i] >= '0' && t[i] <= '9') digits++;
        else if (t[i] != '.' && !(i == 0 && (t[i] == '-' || t[i] == '+'))) return 0;
    }
    return digits > 0;
}

/* One line per word (or rest, or tempo): a line break before every token that is not a note or a number. The
 * same score written over several lines comes out unchanged in meaning. */
static char *score_reflow(const char *s)
{
    sbuf o = {0};
    const char *p = s;
    int first = 1;
    sb_puts(&o, "");
    while (*p) {
        size_t n;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        n = strcspn(p, " \t\r\n");
        if (!first) sb_putc(&o, score_cont(p, n) ? ' ' : '\n');
        while (n--) sb_putc(&o, *p++);
        first = 0;
    }
    sb_putc(&o, '\n');
    if (o.oom) {
        free(o.b);
        return NULL;
    }
    return o.b;
}

/* The written rests at the two edges of a score (one line per word / rest / tempo, as score_reflow makes it): their
 * lengths in seconds, with the tempo in effect there (the engine's rules: default 100, 20..1000; beats 0..32). The
 * silence trim would otherwise cut them with the engine's own padding. */
static void score_edge_rests(const char *sc, double *lead, double *trail)
{
    const char *p = sc;
    double tempo = 100.0, run = 0.0;
    int seen_word = 0;
    *lead = *trail = 0.0;
    while (*p) {
        size_t n = strcspn(p, "\n");
        char line[128];
        size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, k);
        line[k] = 0;
        p += n;
        if (*p) p++;
        if (!k) continue;
        if (!strncmp(line, "tempo ", 6)) {
            double t = atof(line + 6);
            if (!(t > 0.0)) t = 100.0;
            tempo = t < 20.0 ? 20.0 : t > 1000.0 ? 1000.0 : t;
        } else if (line[0] == '-' && (line[1] == ' ' || !line[1])) {
            double b = line[1] ? atof(line + 2) : 1.0;
            b = b > 0.0 ? (b < 32.0 ? b : 32.0) : 0.0;
            run += b * 60.0 / tempo;
        } else {
            if (!seen_word) *lead = run;
            seen_word = 1;
            run = 0.0;
        }
    }
    if (seen_word) *trail = run;
    else *lead = run;   /* rests only: all of it before (nothing to trim around) */
}

static int emit_zeros(cv_pcm_fn fn, void *user, double sec)
{
    static const int16_t z[2048];
    size_t n = (size_t)(sec * 22050.0 + 0.5);
    while (n) {
        size_t k = n < 2048 ? n : 2048;
        if (fn(z, k, user)) return 1;
        n -= k;
    }
    return 0;
}

/* ClassicVoices: singing mode (see cv_bridge.h) */
int cv_voice_sing(cv_voice *v, const char *utf8, double sapi_rate, double semitones, const cv_sing_settings *cfg,
                  int literal, int trim, cv_pcm_fn fn, void *user, int *mismatches)
{
    trimmer t;
    sam_callbacks cb;
    sam_sing_cfg sc;
    char *clean;
    int rc;
    if (mismatches) *mismatches = 0;
    if (!v || !utf8 || !fn) return -1;
    if (literal) { /* a score is line-based: sanitize each line on its own and keep the line breaks */
        sbuf sb = {0};
        const char *p = utf8;
        sb_puts(&sb, "");
        while (*p) {
            size_t n = strcspn(p, "\r\n");
            char *line = malloc(n + 1), *c;
            if (!line) break;
            memcpy(line, p, n);
            line[n] = 0;
            c = cv_sanitize_alloc(line);
            free(line);
            if (c) {
                sb_puts(&sb, c);
                free(c);
            }
            sb_putc(&sb, '\n');
            p += n;
            while (*p == '\r' || *p == '\n') p++;
        }
        if (sb.oom) {
            free(sb.b);
            return -1;
        }
        clean = score_reflow(sb.b);   /* SSML turns the score's line breaks into spaces: rebuild the lines */
        free(sb.b);
    } else {
        clean = cv_sanitize_alloc(utf8);
    }
    if (!clean) return -1;
    if (!*clean) {
        free(clean);
        return 0;
    }
    memset(&t, 0, sizeof t);
    t.fn = fn;
    t.user = user;
    t.trim = trim;
    memset(&sc, 0, sizeof sc);
    if (cfg) {
        sc.vibrato_cents = (float)(cfg->vibrato_cents < 0.0 ? 0.0 : cfg->vibrato_cents > 200.0 ? 200.0 : cfg->vibrato_cents);
        sc.vibrato_rate = (float)(cfg->vibrato_rate <= 0.0 ? 5.5 : cfg->vibrato_rate > 12.0 ? 12.0 : cfg->vibrato_rate);
        sc.transpose = cfg->transpose < -24.0 ? -24.0 : cfg->transpose > 12.0 ? 12.0 : cfg->transpose;
    }
    sam_tts_set_rate_pitch_f(v->s, sapi_rate, semitones);
    cb.audio = on_audio;
    cb.event = NULL;
    cb.user = &t;
    {
        double lead = 0.0, trail = 0.0;
        if (literal) score_edge_rests(clean, &lead, &trail);
        /* a written rest at either edge of a sung score is kept (Quinton 01:40): its silence is sent around the trimmed
         * audio, so only the engine's own padding is trimmed */
        if (lead > 0.0 && emit_zeros(fn, user, lead)) t.stopped = 1;
        rc = t.stopped ? 1 : sam_tts_sing_speech(v->s, clean, literal, &sc, &cb, mismatches);
        tr_finish(&t);
        if (rc == 0 && !t.stopped && trail > 0.0 && emit_zeros(fn, user, trail)) t.stopped = 1;
    }
    if (rc == 0 && t.stopped) rc = 1;
    free(t.quiet);
    free(t.out);
    free(clean);
    return rc;
}

int cv_sing_is_score(const char *utf8) { return utf8 ? sam_sing_is_score(utf8) : 0; }

/* ClassicVoices: finding song scores inside ordinary text (see cv_bridge.h). Tokens are split at ASCII whitespace;
 * a token must match exactly (no punctuation attached). */
typedef struct {
    size_t s, n;
} cv_tok;

static int tok_is(const char *t, size_t n, const char *w) /* case-insensitive whole-token compare */
{
    size_t i;
    if (strlen(w) != n) return 0;
    for (i = 0; i < n; i++)
        if ((t[i] | 32) != w[i]) return 0;
    return 1;
}

/* NOTE: an upper-case A-G, an optional # or b, one octave digit ("C4", "F#3", "Bb5") */
static int tok_note(const char *t, size_t n)
{
    size_t i = 1;
    if (n < 2 || n > 3 || t[0] < 'A' || t[0] > 'G') return 0;
    if (n == 3) {
        if (t[1] != '#' && t[1] != 'b') return 0;
        i = 2;
    }
    return t[i] >= '0' && t[i] <= '9';
}

/* BEATS: a plain number (digits, at most one decimal point, not last), greater than 0 and at most 32 */
static int tok_beats(const char *t, size_t n)
{
    size_t i;
    int dots = 0, digits = 0;
    double v = 0, scale = 0;
    if (!n || n > 6) return 0;
    for (i = 0; i < n; i++) {
        if (t[i] == '.') {
            if (dots++) return 0;
            scale = 1;
        } else if (t[i] >= '0' && t[i] <= '9') {
            digits++;
            if (scale > 0) {
                scale /= 10;
                v += (t[i] - '0') * scale;
            } else {
                v = v * 10 + (t[i] - '0');
            }
        } else {
            return 0;
        }
    }
    if (t[n - 1] == '.') return 0; /* "1." ends a sentence, it is not a length */
    return digits > 0 && v > 0 && v <= 32;
}

/* a tempo: a whole number 20..1000 */
static int tok_tempo(const char *t, size_t n)
{
    size_t i;
    int v = 0;
    if (!n || n > 4) return 0;
    for (i = 0; i < n; i++) {
        if (t[i] < '0' || t[i] > '9') return 0;
        v = v * 10 + (t[i] - '0');
    }
    return v >= 20 && v <= 1000;
}

/* a sung word: letters with inner hyphens / apostrophes ("twin-kle", "don't"), 1-40 characters, not a NOTE */
static int tok_lyric(const char *t, size_t n)
{
    size_t i;
    int letters = 0;
    if (!n || n > 40 || tok_note(t, n)) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        if ((c | 32) >= 'a' && (c | 32) <= 'z') letters++;
        else if (!((c == '-' || c == '\'') && i > 0 && i + 1 < n)) return 0;
    }
    return letters > 0;
}

/* the fewest NOTE BEATS pairs that make a score without a "tempo N" opener (Quinton 17:31: "It can always be changed
 * later") */
#define CV_SCORE_MIN_PAIRS 3

int cv_score_find(const char *utf8, cv_score_run *runs, int max)
{
    cv_tok *tk = NULL;
    size_t nt = 0, cap = 0, i = 0, p = 0;
    int nr = 0;
    if (!utf8 || max <= 0) return 0;
    while (utf8[p]) { /* tokenize */
        size_t st;
        while (utf8[p] == ' ' || utf8[p] == '\t' || utf8[p] == '\r' || utf8[p] == '\n') p++;
        if (!utf8[p]) break;
        st = p;
        while (utf8[p] && utf8[p] != ' ' && utf8[p] != '\t' && utf8[p] != '\r' && utf8[p] != '\n') p++;
        if (nt == cap) {
            size_t c = cap ? cap * 2 : 64;
            cv_tok *n2 = realloc(tk, c * sizeof *tk);
            if (!n2) {
                free(tk);
                return 0;
            }
            tk = n2;
            cap = c;
        }
        tk[nt].s = st;
        tk[nt].n = p - st;
        nt++;
    }
#define T(k) (utf8 + tk[k].s), tk[k].n
    while (i < nt && nr < max) {
        size_t j = i;
        int pairs = 0, tempo = 0;
        if (j + 1 < nt && tok_is(T(j), "tempo") && tok_tempo(T(j + 1))) {
            tempo = 1;
            j += 2;
        }
        for (;;) {
            if (j + 1 < nt && tk[j].n == 1 && utf8[tk[j].s] == '-' && tok_beats(T(j + 1))) { /* rest */
                j += 2;
                continue;
            }
            if (j + 2 < nt && tok_lyric(T(j)) && tok_note(T(j + 1)) && tok_beats(T(j + 2))) { /* word NOTE BEATS ... */
                j += 1;
                while (j + 1 < nt && tok_note(T(j)) && tok_beats(T(j + 1))) {
                    j += 2;
                    pairs++;
                }
                continue;
            }
            break;
        }
        if (pairs >= CV_SCORE_MIN_PAIRS || (tempo && pairs >= 1)) {
            runs[nr].start = tk[i].s;
            runs[nr].len = tk[j - 1].s + tk[j - 1].n - tk[i].s;
            nr++;
            i = j;
        } else {
            i++;
        }
    }
#undef T
    free(tk);
    return nr;
}

/* Moved here unchanged from cv4_bridge.c (2026-09-26): the SAPI 4 bridge and the TruVoice bridge (cvt_bridge.c) both use it. */
char *cv4_to_cp1252_alloc(const char *utf8)
{
    const unsigned char *p = (const unsigned char *)utf8;
    size_t len = strlen(utf8), i = 0, o = 0;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    while (i < len) {
        unsigned c = p[i], cp = 0;
        int extra = 0, k;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xc2 && c < 0xe0) {
            extra = 1;
            cp = c & 0x1f;
        } else if (c >= 0xe0 && c < 0xf0) {
            extra = 2;
            cp = c & 0x0f;
        } else if (c >= 0xf0 && c < 0xf5) {
            extra = 3;
            cp = c & 0x07;
        } else {
            i++;
            out[o++] = ' ';
            continue;
        }
        if (extra && i + (size_t)extra >= len) {
            i++;
            out[o++] = ' ';
            continue;
        }
        for (k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xc0) != 0x80) break;
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if (k <= extra) {
            i++;
            out[o++] = ' ';
            continue;
        }
        i += (size_t)extra + 1;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) out[o++] = (char)cp;
        else if (cp == 0x20AC) out[o++] = (char)0x80; /* euro */
        else if (cp == 0x2122) out[o++] = (char)0x99; /* trade mark */
        else if (cp == 0x2030) out[o++] = (char)0x89; /* per mille */
        else out[o++] = ' ';
    }
    out[o] = 0;
    return out;
}
