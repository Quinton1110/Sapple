#include "fe_input.h"
#include "fe_small.h"
#include "fe_split.h"
#include "tags.h"
#include "queue.h"
#include "voice.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAYOUT(FeEntry, text, 8);
LAYOUT(FrontEnd, words, 0x24);
LAYOUT(FrontEnd, text, 0x34);
LAYOUT(FrontEnd, addr_mode, 0x48);
LAYOUT(FrontEnd, single_char, 0x88);
LAYOUT(FrontEnd, abort_event, 0x70);
LAYOUT(FrontEnd, queue, 0x7c);
LAYOUT(FrontEnd, _8c, 0x8c);

void fe_entries_clear(FrontEnd *fe) {
    while (fe->last >= 0) {
        FeEntry *e = &GP(FeEntry, fe->entries)[fe->last];
        if (e->text) vc_free(GP(char, e->text));
        GP(FeEntry, fe->entries)[fe->last].text = 0;
        fe->last--;
    }
    fe->last = -1;
}

static FeEntry *fe_entry_new(FrontEnd *fe) {
    fe->last++;
    if (fe->last == fe->cap - 1) {
        FeEntry *n = vc_malloc((size_t)(uint32_t)fe->cap * 2 * sizeof(FeEntry));
        memcpy(n, GP(FeEntry, fe->entries), (size_t)(uint32_t)fe->cap * sizeof(FeEntry));
        vc_free(GP(FeEntry, fe->entries));
        GPSET(fe->entries, n);
        fe->cap *= 2;
        n[fe->cap - 1].pos = 2000000000;
    }
    return &GP(FeEntry, fe->entries)[fe->last];
}

static void fe_entry_text(FrontEnd *fe, const char *s) {
    char *t = vc_malloc(strlen(s) + 1);
    GPSET(GP(FeEntry, fe->entries)[fe->last].text, t);
    strcpy(t, s);
}

int32_t fe_entry_add_num(FrontEnd *fe, int32_t type, uint32_t value) {
    char buf[0x50];
    fe_entry_new(fe)->type = type;
    sprintf(buf, DLLVAR(const char, 0x6371f120), value);     // "%u"
    fe_entry_text(fe, buf);
    return fe->last;
}

int32_t fe_entry_add_str(FrontEnd *fe, int32_t type, const char *s) {
    fe_entry_new(fe)->type = type;
    fe_entry_text(fe, s);
    return fe->last;
}

// two tables of { name, value } in .data
GPTR(const char) tag_mode_name(int32_t v) {
    const uint32_t *t = DLLVAR(const uint32_t, 0x6371f378);
    for (int i = 0; i < 17; i++)
        if ((int32_t)t[2 * i + 1] == v) return DLLPTR(const char, 0x6371f378)[2 * i];
    return 0;
}

int32_t tag_mode_value(const char *name) {
    for (int i = 0; i < 8; i++)
        if (vc_lstrcmpiA(name, GP(const char, DLLPTR(const char, 0x6371f400)[2 * i])) == 0)
            return DLLVAR(const int32_t, 0x6371f404)[2 * i];
    return (int32_t)0x80000000u;
}

void tag_skip(GPTR(char) *pp) {
    while (*GP(char, *pp) != '\\' && *GP(char, *pp) != 0) GPSET(*pp, GP(char, *pp) + 1);
    if (*GP(char, *pp)) GPSET(*pp, GP(char, *pp) + 1);
}

void str_append(char **cur, const char *s) {
    char *d = *cur;
    size_t i = 0;
    do d[i] = s[i];
    while (s[i++]);
    *cur = d + i - 1;
}

static int lex(char **p, char *buf, int n) {
    GPTR(const char) g;
    GPSET(g, *p);
    int t = tag_lex(&g, buf, n);
    *p = (char *)GP(const char, g);
    return t;
}

#define ESC_N DLLVAR(const char, 0x6371f194)        // "\x1bN%d "
#define ADDR_ON DLLVAR(const char, 0x6371f19c)      // "\x1b[addr] "
#define ADDR_OFF DLLVAR(const char, 0x6371f16c)     // "\x1b[\\addr] "
#define POS(p) ((uint32_t)((p) - text) * ((mode == 0 || mode == 1) ? 2u : 1u))

int32_t fe_tags_to_escapes(FrontEnd *fe, int32_t tags, int32_t mode, char *text, GPTR(char) *out) {
    int32_t tagsize = 0x50, pending = 0, toggle = 0, nbs = 0, nwords = 0, num = 0, mark = 0;
    char *tagbuf = vc_malloc(0x50);
    char lexbuf[0x50];
    char buf[0x50] = "";    // (the original's is uninitialised until the first escape is formatted)
    char *bm_end = NULL;
    char *p;
    for (p = text; *p; p++)
        if (*p == 0x1b) *p = ' ';
    for (p = text; *p; p++)
        if (tags && *p == '\\') {
            nbs++;
            toggle = ~toggle;
        }
    uint32_t len = (uint32_t)strlen(text);
    toggle = 0;
    char *q = text, *end = text + len;
    while (*q) {
        q += strcspn(q, DLLVAR(const char, 0x6371f1b0));     // "\\ \t\n"
        if (!*q) break;
        if (*q == '\\') {
            if (tags) toggle = ~toggle;
            q++;
        } else if (toggle) q++;
        else {
            *q++ = 0;       // a word boundary outside tags (put back as a space below)
            nwords++;
        }
    }
    int32_t size = (int32_t)(len + (uint32_t)(nbs + nwords) * 8u + 0x1a);
    char *o = vc_malloc((size_t)(uint32_t)size);
    GPSET(*out, o);
    strcpy(o, DLLVAR(const char, 0x6371f1a8));             // "\x1bAP "
    char *cur = o + 4;
    toggle = 0;
    if (fe->addr_mode == 2) {
        sprintf(buf, "%s", ADDR_ON);
        str_append(&cur, buf);
    }
    p = text;
    while (p < end) {
        if (size - (int32_t)(cur - GP(char, *out)) < 0x14) {
            size = size * 2 + 0x14;
            char *n = vc_realloc(GP(char, *out), (size_t)(uint32_t)size);
            cur += n - GP(char, *out);
            GPSET(*out, n);
        }
        if (*p == 0) *p = p[-1] == '\r' ? '\n' : ' ';
        if (pending && p >= bm_end) {
            sprintf(buf, ESC_N, fe_entry_add_num(fe, 0x14, 0));
            str_append(&cur, buf);
            pending = 0;
        }
        if (!(tags && *p == '\\')) {
            char c = *p;
            if (c == ' ' || c == '\n') toggle = 0;
            else if (c != '\t' && c != '\r' && toggle == 0) {
                toggle = 1;
                sprintf(buf, ESC_N, fe_entry_add_num(fe, 1, POS(p)));
                str_append(&cur, buf);
            }
            *cur++ = *p++;
            *cur = 0;
            continue;
        }
        // a tag
        toggle = 0;
        p++;
        int32_t tok = lex(&p, lexbuf, 0x50);
        switch (tok) {
        case 0:
            sprintf(buf, ESC_N, fe_entry_add_str(fe, 0, GP(const char, tag_mode_name(0))));
            str_append(&cur, buf);
            if (lex(&p, lexbuf, 0x50) != TOK_EQ) goto skip;
            for (;;) {
                if (lex(&p, tagbuf, tagsize) != TOK_STRING) goto skip;
                sprintf(buf, ESC_N, fe_entry_add_str(fe, 0, tagbuf));
                str_append(&cur, buf);
                int32_t t = lex(&p, lexbuf, 0x50);
                if (t == TOK_BACKSLASH) goto next;
                if (t != TOK_COMMA) goto skip;
            }
        case 4:
            str_append(&cur, DLLVAR(const char, 0x6371f190));  // "\x1b\" "
            goto skip;
        case 9:
        case 0xa:
        case 0xb:
        case 0x10:
        case 0x12:
            if (lex(&p, lexbuf, 0x50) != TOK_EQ) goto skip;
            if (lex(&p, (char *)&num, 4) != TOK_NUMBER) goto skip;
            if (lex(&p, lexbuf, 0x50) != TOK_BACKSLASH) goto skip;
            if (tok == 0xa || tok == 0xb) {
                sprintf(buf, DLLVAR(const char, tok == 0xa ? 0x6371f178 : 0x6371f180), (uint32_t)num);  // H / I
                str_append(&cur, buf);
            }
            sprintf(buf, ESC_N, fe_entry_add_num(fe, tok, (uint32_t)num));
            str_append(&cur, buf);
            goto next;
        case 0xf: {
            int32_t old = fe->addr_mode;
            fe->addr_mode = 0;
            if (old == 2) {
                sprintf(buf, "%s", ADDR_OFF);
                str_append(&cur, buf);
            }
            str_append(&cur, DLLVAR(const char, 0x6371f164));     // "\x1bI0 "
            sprintf(buf, ESC_N, fe_entry_add_num(fe, 0xf, 0));
            str_append(&cur, buf);
            goto skip;
        }
        case 3:
        case 0xe:
            if (lex(&p, lexbuf, 0x50) != TOK_EQ) goto skip;
            if (lex(&p, tagbuf, tagsize) != TOK_STRING) goto skip;
            if (lex(&p, lexbuf, 0x50) != TOK_BACKSLASH) goto skip;
            if (tok == 3) {
                int32_t old = fe->addr_mode;
                int32_t v = tag_mode_value(tagbuf);
                if (v == (int32_t)0x80000000u) goto next;
                if (v == 2) {
                    fe->addr_mode = 2;
                    if (old == 2) goto next;
                    sprintf(buf, "%s", ADDR_ON);
                } else {
                    fe->addr_mode = 0;
                    if (old != 2) goto next;
                    sprintf(buf, "%s", ADDR_OFF);
                }
            } else {
                // a part of speech
                static const uint32_t pos[5][2] = { { 0x6371f160, 0x6371f134 }, { 0x6371f15c, 0x6371f154 },
                    { 0x6371f14c, 0x6371f144 }, { 0x6371f13c, 0x6371f134 }, { 0x6371f12c, 0x6371f124 } };
                for (int i = 0; i < 5; i++)
                    if (vc_lstrcmpiA(DLLVAR(const char, pos[i][0]), tagbuf) == 0) {
                        sprintf(buf, "%s", DLLVAR(const char, pos[i][1]));
                        break;
                    }
                // (none: the escape formatted last is appended again)
            }
            str_append(&cur, buf);
            goto next;
        case 0x13: {
            if (pending) {
                sprintf(buf, ESC_N, fe_entry_add_num(fe, 0x14, 0));
                str_append(&cur, buf);
                pending = 0;
            }
            if (vc_scan_eq_int(p, &mark) != 1) {
                char *b = strchr(p, '\\');
                p = b ? b + 1 : end;
                goto next;
            }
            char *b = strchr(p, '\\');
            if (!b) {
                p = end;
                goto next;
            }
            uint32_t v = (uint32_t)(p - text);
            if (mode == 0 || mode == 1) v = (uint32_t)-v << 1;
            sprintf(buf, ESC_N, fe_entry_add_num(fe, 0x13, v));
            str_append(&cur, buf);
            pending = 1;
            int32_t n = (int32_t)(b - p) + 1;
            if (n > tagsize) {
                tagsize = n;
                tagbuf = vc_realloc(tagbuf, (size_t)(uint32_t)n);
            }
            strncpy(tagbuf, p, (size_t)(b - p));
            tagbuf[b - p] = 0;
            sprintf(buf, ESC_N, fe_entry_add_str(fe, 0x15, tagbuf));
            str_append(&cur, buf);
            p = b + 1;
            bm_end = p + mark;
            goto next;
        }
        case TOK_BACKSLASH:
            // an empty tag: a backslash to speak
            toggle = 1;
            sprintf(buf, ESC_N, fe_entry_add_num(fe, 1, POS(p)));
            str_append(&cur, buf);
            str_append(&cur, DLLVAR(const char, 0x6371f18c));     // "\\"
            goto next;
        default:
            goto skip;
        }
    skip: {
            GPTR(char) g;
            GPSET(g, p);
            tag_skip(&g);
            p = GP(char, g);
        }
    next:;
    }
    if (fe->addr_mode == 2) {
        sprintf(buf, "%s", ADDR_OFF);
        str_append(&cur, buf);
    }
    vc_free(tagbuf);
    return (int32_t)strlen(GP(char, *out));
}

int32_t fe_set_text(FrontEnd *fe, char *text) {
    char *t = vc_malloc(strlen(text) + 3);
    GPSET(fe->text, t);
    strcpy(t, text);
    fe->cur = fe->text;
    char *last = t + strlen(t) - 1;
    GPSET(fe->text_last, last);
    last[2] = 0;
    char *n = t;
    GPSET(fe->next, n);
    while (n <= last) {
        if (*n == 0x1b && n[1] == 'I') break;
        n++;
        GPSET(fe->next, n);
    }
    *GP(char, fe->next) = 0;
    int32_t r = words_feed(fe->words, GP(char, fe->text), &fe->f28, &fe->f2c);
    return fe_after_feed(fe, r);
}

static uint32_t wlen(const uint16_t *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

int32_t fe_text_input(FrontEnd *fe, const uint16_t *text, int32_t *out, int32_t tags, int32_t mode) {
    GPTR(uint16_t) wide = 0;
    GPTR(char) conv = 0;
    const uint16_t *w = text;
    if (mode == 1) {
        phonemes_to_names(text, &wide, tags);
        w = GP(uint16_t, wide);
    }
    uint32_t n = wlen(w);
    int32_t size = 1 - x87_ftol((double)n * -1.1);
    char *a = vc_malloc((size_t)(uint32_t)size);
    // WideCharToMultiByte(CP_ACP, 0, w, -1, a, size, NULL, NULL)
    if (size >= (int32_t)n + 1)
        for (uint32_t i = 0; i <= n; i++) a[i] = (char)uni_to_cp1252(w[i]);
    if (mode == 1) free_if2(GP(uint16_t, wide));
    if (strlen(a) == 1) fe->single_char = 1;
    fe_entries_clear(fe);
    fe_tags_to_escapes(fe, tags, mode, a, &conv);
    char *res;
    if (mode != 0 && mode != 4) {
        GPTR(char) r = 0;
        text_from_other(GP(char, conv), &r);
        res = GP(char, r);
    } else res = text_prepare(GP(char, conv), fe->single_char);
    vc_free(GP(char, conv));
    *out = fe_set_text(fe, res);
    vc_free(a);
    if (mode == 0) vc_free(res);
    else free_if(res);
    return 0;
}

int32_t phoneme_index(uint16_t c) {
    const uint16_t *t = DLLVAR(const uint16_t, 0x6369e5e2);
    for (int i = 0; i < 53; i++)
        if (t[i] == c) return i + 1;
    return 0;
}

int32_t phonemes_to_names(const uint16_t *text, GPTR(uint16_t) *out, int32_t tags) {
    uint16_t tmp[2] = { 0, 0 };
    uint16_t *o = vc_calloc(2u * wlen(text) + 1u, 2);
    GPSET(*out, o);
    o[0] = 0;
    int32_t intag = 0;
    for (const uint16_t *p = text; *p; p++) {
        const uint16_t *add;
        if (*p == '\\') {
            if (!tags) return -1;
            tmp[0] = '\\';
            intag = intag == 0;
            add = tmp;
        } else if (intag) {
            tmp[0] = *p;
            add = tmp;
        } else {
            int32_t k = phoneme_index(*p);
            if (k <= 0) return -1;
            add = DLLVAR(const uint16_t, 0x6369e498) + 3 * k;
        }
        uint16_t *d = o + wlen(o);
        uint32_t i = 0;
        do d[i] = add[i];
        while (add[i++]);
    }
    return 0;
}

int32_t free_if2(void *p) { return free_if(p); }

// ------------------------------------------------------------------ after the word module read a part

// an escape's letter at *p, then its argument up to and including the space after it into buf (as the
// original does it: nothing is written when the escape has no argument); where it stopped
static char *esc_arg(char *p, char *buf) {
    char c = *p;
    if (!c) return p;
    int k = 0;
    while (c != ' ') {
        c = *++p;
        buf[k] = c;
        buf[k + 1] = 0;
        c = *p;
        k++;
        if (!c) break;
    }
    return p;
}

int32_t tag_char_value(const char *name) {
    for (int i = 0; i < 17; i++)
        if (vc_lstrcmpiA(name, GP(const char, DLLPTR(const char, 0x6371f378)[2 * i])) == 0)
            return DLLVAR(const int32_t, 0x6371f37c)[2 * i];
    return (int32_t)0x80000000u;
}

uint32_t voice_rec_count(const uint8_t *v) { return *(const uint32_t *)(const void *)(v + 0xb18); }

const uint8_t *voice_rec(const uint8_t *v, uint32_t i, int32_t *key, int32_t *len) {
    const uint8_t *r = v + 0xb1c;
    for (; i; i--) r += *(const int32_t *)(const void *)(r + 4) + 8;
    *key = *(const int32_t *)(const void *)r;
    *len = *(const int32_t *)(const void *)(r + 4);
    return r + 8;
}

void fe_voice_flags(FrontEnd *fe, int32_t v) {
    if (v == 0) {
        fe->flags &= ~0x100u;
        return;
    }
    const ModeTable *mt = GP(const ModeTable, GP(GPTR(const ModeTable), *DLLPTR(GPTR(const ModeTable), 0x63738bc0))[fe->voice]);
    const uint8_t *d = GP(const uint8_t, GP(GPTR(const uint8_t), mt->modes)[v]);
    uint32_t n = voice_rec_count(d);
    for (uint32_t i = 0; i < n; i++) {
        int32_t key, len;
        voice_rec(d, i, &key, &len);
        if ((key & 0xffffff) == 5) fe->flags |= 0x100u;
    }
}

int32_t fe_text_end(FrontEnd *fe) {
    fe->pitch_set = fe->pitch;
    words_set_pitch(fe->words, fe->pitch);
    vc_PostMessageA(fe->hwnd, fe->msg, 1, 0);
    return 0;
}

int32_t fe_after_feed(FrontEnd *fe, int32_t r) {
    // the original's 20-byte buffer with its "only escapes" flag right after it (a longer argument
    // runs into the flag, as there)
    uint8_t frame[0x100];
    memset(frame, 0, sizeof frame);
    char *buf = (char *)frame;
    int32_t one = 1;
    memcpy(frame + 0x14, &one, 4);
    char *cur = GP(char, fe->cur);
    char *p = cur + fe->f28, *last = cur + fe->f2c;
    if (last > GP(char, fe->next)) last = GP(char, fe->next);
    fe->nindex = 0;
    if (*last == 0) last--;
    while (p <= last) {
        char c = *p;
        if (c == ' ' || c == '\r') {
            p++;
            continue;
        }
        if (c == 0x1b) {
            char letter = *++p;
            p = esc_arg(p, buf);
            if (letter == 'N') {
                if (fe->nindex == 0) fe->first_index = vc_atoi(buf);
                fe->nindex++;
            }
            continue;
        }
        memset(frame + 0x14, 0, 4);
        while (c && c != ' ' && c != 0x1b) c = *++p;
    }
    int32_t ok;
    memcpy(&ok, frame + 0x14, 4);
    if (ok == 0 && r != 0x6f) return 0x70;
    p = cur + fe->f28;
    for (;;) {
        char c = *p;
        if (!c) return 0x6f;
        if (c != 0x1b) {
            p++;
            continue;
        }
        char letter = *++p;
        p = esc_arg(p, buf);
        if (letter == 'I') {
            if (vc_lstrcmpA(buf, DLLVAR(const char, 0x6371da14)) == 0) fe_text_end(fe);     // "0"
            continue;
        }
        if (letter != 'N') continue;
        int32_t idx = vc_atoi(buf);
        TagOut *t = vc_malloc(sizeof(TagOut));
        if (!t) continue;
        memset(t, 0, sizeof(TagOut));
        FeEntry *e = &GP(FeEntry, fe->entries)[idx];
        const char *fmt = NULL;
        switch (e->type) {
        case 0: {
            int32_t v = tag_char_value(GP(const char, e->text));
            if (v == 0) {
                fe->flags = 0;
                if (fe->f50 < 0) fe_voice_flags(fe, -fe->f50);
            } else if (v == 0x100) fe->flags |= 0x100u;
            fmt = DLLVAR(const char, 0x6371f1fc);       // "\Chr=%s\\n"
            break;
        }
        case 1: fmt = DLLVAR(const char, 0x6371f208); break;       // "\Wrd=%s\\n"
        case 9: fmt = DLLVAR(const char, 0x6371f1f0); break;       // "\Mrk=%s\\n"
        case 0xa: fmt = DLLVAR(const char, 0x6371f1b8); break;     // "\Pau=%s\\n"
        case 0xb: fmt = DLLVAR(const char, 0x6371f1c4); break;     // "\Pit=%s\\n"
        case 0xf:
            sprintf(buf, "%s", DLLVAR(const char, 0x6371f1d0));    // "\Rst\\n"
            fe->flags = 0;
            if (fe->f50 < 0) fe_voice_flags(fe, -fe->f50);
            break;
        case 0x10: fmt = DLLVAR(const char, 0x6371f1d8); break;    // "\Spd=%s\\n"
        case 0x12: fmt = DLLVAR(const char, 0x6371f1e4); break;    // "\Vol=%s\\n"
        default: break;         // (the escape's own argument is parsed as the tag)
        }
        if (fmt) sprintf(buf, fmt, GP(const char, GP(FeEntry, fe->entries)[idx].text));
        tag_parse(buf, t);
        QItem q;        // (the same 20 bytes in the original; field by field for a portable build)
        q.code = (uint32_t)t->code;
        q.a.value = (uint32_t)t->value;
        q.v2 = (uint32_t)t->_08;
        q.data = t->text;
        q.bytes = (uint32_t)t->text_bytes;
        queue_push(GP(Queue, fe->queue), &q);
        vc_free(t);
    }
}

int32_t fe_continue(FrontEnd *fe) {
    int32_t r = words_continue(fe->words, &fe->f28, &fe->f2c);
    if (GP(char, fe->cur)[fe->f28] == 0) return 0x6f;
    return fe_after_feed(fe, r);
}
