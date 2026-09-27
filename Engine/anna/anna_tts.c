/* Microsoft Anna text to speech: library interface (see anna_tts.h). */
#include "anna_tts.h"
#include "anna.h"
#include "anna_front.h"
#include "anna_lex.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS (1 << 14)
#define MAX_UNITS (1 << 15)

struct anna_tts {
    anna_decoder *dec;
    anna_voice *voice;
    anna_mid *mid;
    anna_udt *udt;
    int rate, volume, pitch;
    double rate_frac; /* ClassicVoices patch: rate + rate_frac is the SAPI rate */
    volatile int cancel;
    anna_word *words;
    anna_unitspec *units;
    anna_unitinfo *info;
};

static int file_exists(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int read_at(const char *fn, long off, void *buf, size_t n)
{
    FILE *f = fopen(fn, "rb");
    int ok;
    if (!f) return -1;
    ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

anna_tts *anna_tts_open(const char *dir, char *err, size_t errlen)
{
    anna_tts *t = calloc(1, sizeof *t);
    char base[1024], fn[1024], key_file[1024], han_file[1024];
    unsigned char key[129];
    long key_off, han_off;
    if (!t) return NULL;
    snprintf(fn, sizeof fn, "%s/en-US/enu-dsk/M1033DSK.UNT", dir);
    if (file_exists(fn)) { /* the installed TTS20 folder */
        snprintf(base, sizeof base, "%s/en-US/enu-dsk/M1033DSK", dir);
        snprintf(key_file, sizeof key_file, "%s/MSTTSDecWrp.dll", dir);
        snprintf(han_file, sizeof han_file, "%s/MSTTSEngine.dll", dir);
        key_off = 0x13c8;
        han_off = 0xd88;
    } else { /* a flat data folder (anna_extract) */
        snprintf(base, sizeof base, "%s/M1033DSK", dir);
        snprintf(key_file, sizeof key_file, "%s.KEY", base);
        snprintf(han_file, sizeof han_file, "%s.HAN", base);
        key_off = 0;
        han_off = 0;
    }
    if (read_at(key_file, key_off, key, sizeof key) || memcmp(key, "wyClImqD", 8)) {
        snprintf(err, errlen, "cannot read the voice key from %s", key_file);
        goto fail;
    }
    t->words = malloc(sizeof *t->words * MAX_WORDS);
    t->units = malloc(sizeof *t->units * MAX_UNITS);
    t->info = malloc(sizeof *t->info * MAX_UNITS);
    if (!t->words || !t->units || !t->info) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    {
        char csd[1100], idx[1100], udt[1100];
        snprintf(csd, sizeof csd, "%s.CSD", base);
        snprintf(idx, sizeof idx, "%s.IDX", base);
        snprintf(udt, sizeof udt, "%s.UDT", base);
        if (!(t->dec = anna_decoder_open(csd, idx, key, err, errlen)) || !(t->voice = anna_voice_load(base, t->dec, err, errlen)) ||
            !(t->mid = anna_mid_new(base, err, errlen)) || !(t->udt = anna_udt_load(udt, err, errlen)))
            goto fail;
    }
    anna_voice_load_pitch_table(t->voice, han_file, han_off); /* optional: only <pitch> and set_pitch need it */
    t->volume = 100;
    return t;
fail:
    anna_tts_close(t);
    return NULL;
}

void anna_tts_close(anna_tts *t)
{
    if (!t) return;
    if (t->udt) anna_udt_free(t->udt);
    if (t->mid) anna_mid_free(t->mid);
    if (t->voice) anna_voice_free(t->voice);
    if (t->dec) anna_decoder_close(t->dec);
    free(t->words);
    free(t->units);
    free(t->info);
    free(t);
}

void anna_tts_set_rate(anna_tts *t, int rate) { t->rate = rate < -10 ? -10 : rate > 18 ? 18 : rate; t->rate_frac = 0; }
void anna_tts_set_rate_f(anna_tts *t, double rate) /* ClassicVoices patch */
{
    double r = rate == rate ? (rate < -10 ? -10 : rate > 18 ? 18 : rate) : 0, whole = floor(r);
    t->rate = (int)whole;
    t->rate_frac = r - whole;
}
void anna_tts_set_volume(anna_tts *t, int volume) { t->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume; }
void anna_tts_set_pitch(anna_tts *t, int pitch) { t->pitch = pitch < -10 ? -10 : pitch > 10 ? 10 : pitch; }
void anna_tts_cancel(anna_tts *t) { t->cancel = 1; }

/* the normalizer's text positions are UTF-16 units of the input; map them back to UTF-8 byte offsets the same
 * way anna_norm_new decodes (invalid bytes are one Windows-1252 unit) */
static long *utf16_to_utf8_map(const char *s, long *nunits)
{
    size_t len = strlen(s), i = 0;
    long *m = malloc(sizeof(long) * (2 * len + 2)), n = 0;
    if (!m) return NULL;
    while (i < len) {
        unsigned c = (unsigned char)s[i], cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0, ok = 0, k;
        if (extra) {
            ok = 1;
            cp = c & (extra == 3 ? 7 : extra == 2 ? 15 : 31);
            for (k = 1; k <= extra; k++) {
                unsigned d = i + (size_t)k < len ? (unsigned char)s[i + (size_t)k] : 0;
                if ((d & 0xc0) != 0x80) ok = 0;
                else cp = (cp << 6) | (d & 0x3f);
            }
        }
        m[n++] = (long)i;
        if (ok) {
            if (cp > 0xffff) m[n++] = (long)i;
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
    anna_tts *t;
    const anna_callbacks *cb;
    uint64_t base;          /* audio samples delivered so far */
    uint64_t sent_base;     /* audio samples before this sentence */
    long last_word;         /* text position of the last word event (expansions repeat it) */
    char bm_name[512];
    const long *map;
    long nmap;
    const anna_token *tok;  /* bookmarks of this sentence */
    int ntok, next_bm;
    int stopped;
} speak_ctx;

static long map_pos(const speak_ctx *c, long u) { return u < 0 ? 0 : u > c->nmap ? c->map[c->nmap] : c->map[u]; }

static void emit(speak_ctx *c, int type, uint64_t pos, long upos, long ulen, const char *name)
{
    anna_event e;
    if (!c->cb->event) return;
    e.type = type;
    e.audio_pos = pos;
    e.text_pos = map_pos(c, upos);
    e.text_len = map_pos(c, upos + ulen) - e.text_pos;
    e.name = name;
    c->cb->event(&e, c->cb->user);
}

/* bookmarks up to text position upos */
static void emit_bookmarks(speak_ctx *c, uint64_t pos, long upos)
{
    for (; c->next_bm < c->ntok; c->next_bm++) {
        const anna_token *k = &c->tok[c->next_bm];
        if (k->kind != 3) continue;
        if (upos >= 0 && k->src_pos > upos) break;
        { /* the name is the item text (UTF-16) */
            int i, o = 0;
            for (i = 0; k->item_text[i] && o < (int)sizeof c->bm_name - 4; i++) {
                unsigned u = k->item_text[i];
                if (u < 0x80) c->bm_name[o++] = (char)u;
                else if (u < 0x800) c->bm_name[o++] = (char)(0xc0 | u >> 6), c->bm_name[o++] = (char)(0x80 | (u & 63));
                else c->bm_name[o++] = (char)(0xe0 | u >> 12), c->bm_name[o++] = (char)(0x80 | (u >> 6 & 63)), c->bm_name[o++] = (char)(0x80 | (u & 63));
            }
            c->bm_name[o] = 0;
        }
        emit(c, ANNA_EV_BOOKMARK, pos, k->src_pos, 0, c->bm_name);
    }
}

static void on_item(void *user, int item, long long pos)
{
    speak_ctx *c = user;
    const anna_unitinfo *in = &c->t->info[item];
    uint64_t at = c->sent_base + (uint64_t)pos;
    if (item == 0) return; /* the engine's leading copy of the first unit carries no events */
    if (in->ctrl & 2) emit(c, ANNA_EV_SENTENCE, at, in->sent_pos, in->sent_len, NULL);
    if (in->ctrl & 1) {
        emit_bookmarks(c, at, in->src_pos);
        if (in->src_pos != c->last_word) emit(c, ANNA_EV_WORD, at, in->src_pos, in->src_len, NULL);
        c->last_word = in->src_pos;
    }
}

static void on_pcm(const int16_t *pcm, size_t n, void *user)
{
    speak_ctx *c = user;
    if (c->stopped) return;
    if (c->cb->audio && c->cb->audio(pcm, n, c->cb->user)) {
        c->stopped = 1;
        c->t->cancel = 1;
    }
    c->base += n;
}

int anna_tts_speak(anna_tts *t, const char *utf8, int flags, const anna_callbacks *cb)
{
    speak_ctx c;
    anna_norm *nm;
    anna_render_opts o;
    int rc = 0;
    memset(&c, 0, sizeof c);
    t->cancel = 0;
    c.t = t;
    c.cb = cb;
    c.last_word = -1;
    c.map = utf16_to_utf8_map(utf8, &c.nmap);
    nm = anna_norm_new(utf8, (flags & ANNA_SPEAK_XML ? ANNA_NORM_XML : 0) | (flags & ANNA_SPEAK_PUNCTUATION ? ANNA_NORM_SPEAK_PUNC : 0));
    if (!c.map || !nm) {
        free((void *)c.map);
        if (nm) anna_norm_free(nm);
        return -1;
    }
    memset(&o, 0, sizeof o);
    o.sapi_rate = t->rate;
    o.rate_frac = t->rate_frac; /* ClassicVoices patch */
    o.allow_fast = t->rate + t->rate_frac > 10;
    o.sapi_volume = t->volume;
    o.pitch_offset = t->pitch;
    o.cancel = &t->cancel;
    o.item_cb = on_item;
    o.user = &c;
    anna_mid_reset(t->mid); /* one Speak() */
    while (!t->cancel) {
        const anna_token *tok;
        int n, nw, nu, r = anna_norm_sentence(nm, &tok, &n);
        if (r == 0) break;
        if (r < 0) {
            rc = -1;
            break;
        }
        c.sent_base = c.base;
        c.tok = tok;
        c.ntok = n;
        c.next_bm = 0;
        nw = anna_mid_sentence(t->mid, tok, n, t->words, MAX_WORDS);
        if (nw < 0) break; /* a word the engine cannot pronounce: it stops speaking, like the original */
        nu = anna_words_to_units(t->udt, t->words, nw, t->units, t->info, MAX_UNITS);
        if (nu > 0) {
            r = anna_render_sentence_ex(t->voice, t->units, nu, &o, on_pcm, &c);
            if (r < 0) {
                rc = -1;
                break;
            }
        }
        if (!t->cancel) emit_bookmarks(&c, c.base, -1); /* bookmarks after the last word */
    }
    if (t->cancel || c.stopped) rc = rc < 0 ? rc : 1;
    else if (rc == 0 && cb->event) {
        anna_event e;
        memset(&e, 0, sizeof e);
        e.type = ANNA_EV_END;
        e.audio_pos = c.base;
        e.text_pos = (long)strlen(utf8);
        cb->event(&e, cb->user);
    }
    anna_norm_free(nm);
    free((void *)c.map);
    return rc;
}
