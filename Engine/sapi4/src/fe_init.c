// Setting up the front end: the text rules' character classes, the word objects (the word module's
// state per voice), the user lexicons, the per-voice input buffers, and the front end object itself.
#include "fe_init.h"
#include "fe_input.h"
#include "fe_phrase.h"
#include "fe_word.h"
#include "fe_lexer.h"
#include "crt_vc.h"
#include "vcrt.h"
#include <stdlib.h>
#include <string.h>

#define WMAGIC (*DLLVAR(int32_t, 0x63738bd4))
#define WOBJS DLLPTR(uint8_t, 0x63739e9c)       // [1..128]
#define CS DLLVAR(uint8_t, 0x63738b90)

int32_t text_init(uint32_t unused, const char *cfg, uint32_t unused2) {
    (void)unused; (void)unused2;
    memset(DLLVAR(uint8_t, 0x637399d8), 0, 0x414);
    if (cfg) memcpy(DLLVAR(uint8_t, 0x637399d8), cfg, 10);
    lex_classes();
    return 0;
}

void fe_once(void) {
    if (++*DLLVAR(int32_t, 0x6371efc4) == 0) text_init(0x6371efc8, NULL, 0);    // (InterlockedIncrement)
}

void ev_list_init(uint8_t *w) {
    WPTR(w, 0xb4ac) = 0;
    WPTR(w, 0xb4b0) = 0;
    W32(w, 0xb4b4) = 0;
    W16(w, 0xb4b8) = 0;
}

int32_t word_obj_init(uint8_t *w) {
    if ((uint16_t)W16(w, 0xb4ba) == 0x9b39) return -1;
    W32(w, 0xb4c0) = 0xa;
    ev_list_init(w);
    W16(w, 0xb4ba) = (int16_t)0x9b39;
    W16(w, 0xb57c) = 0;
    W16(w, 0xb57a) = 0;
    word_reset(w);
    W32(w, 0xb58c) = 0;
    W32(w, 0xb590) = 0;
    for (int i = 0; i < 0x500; i++) {
        w[0x585 + i] = ' ';
        w[0xb05 + i] = ' ';
        w[0x438e + i] = ' ';
        w[0x1605 + i] = ' ';
        w[0x1085 + i] = ' ';
    }
    return 0;
}

int32_t word_obj_create(void) {
    GPTR(uint8_t) *tab = WOBJS + 1;
    if (WMAGIC != (int32_t)0xffff9bad) {
        WMAGIC = (int32_t)0xffff9bad;
        for (int i = 0; i < 0x80; i++) tab[i] = 0;
    }
    int i = 0;
    while (tab[i]) {
        if (++i >= 0x80) return -0x65;
    }
    uint8_t *w = vc_malloc(WOBJ_ALLOC);
    GPSET(tab[i], w);
    if (!w) return -0x64;
    memset(w, 0, WOBJ_ALLOC);
    word_obj_init(GP(uint8_t, tab[i]));
    return i + 1;
}

static uint8_t *wobj(int32_t idx) {
    if (idx < 1 || idx > 0x80) return NULL;
    return WOBJS[idx] ? GP(uint8_t, WOBJS[idx]) : NULL;
}

int32_t word_set_mask(uint8_t *w, uint8_t m) {
    if (!w || (uint16_t)W16(w, 0xb4ba) != 0x9b39) return -2;
    if (m >= 0x80) return -1;
    W16(w, 0xb4b8) = m;
    return 0;
}

int32_t word_set_events(int32_t idx, int32_t m) {
    if (WMAGIC != (int32_t)0xffff9bad) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    if (m > 0x7f) return -0x68;
    word_set_mask(w, (uint8_t)m);
    return 0;
}

int32_t word_set_mode(int32_t idx, int32_t v) {
    if (WMAGIC != (int32_t)0xffff9bad) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    switch (v) {
    case 0: W32(w, 0xb4d4) = 2; break;
    case 1: W32(w, 0xb4d4) = 0; break;
    case 2: W32(w, 0xb4d4) = 1; break;
    case 3: W32(w, 0xb4d4) = 3; break;
    default: return -0x69;
    }
    return 0;
}

int32_t word_set_rate(int32_t idx, int32_t v) {
    if (WMAGIC != (int32_t)0xffff9bad) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    if (v < 1 || v > 9) return -0x69;
    W16(w, 0xb4cc) = (int16_t)v;
    int16_t r = (int16_t)(((5 - v) << 5) / 8 + 0x20);
    W16(GP(uint8_t, WOBJS[idx]), 0xb588) = r;
    W16(GP(uint8_t, WOBJS[idx]), 0xb58a) = r;
    return 0;
}

void user_lex_builtin(GPTR(UserLex) *out) {
    vc_EnterCriticalSection(CS);
    if (*DLLVAR(int32_t, 0x63738518) == 0) {
        UserLex *t = vc_malloc(sizeof(UserLex));
        GPSET(*out, t);
        t->_0c = 0;
        t->count = *DLLVAR(int32_t, 0x6371d780);
        t->cap = t->count;
        GPSET(t->entries, (GPTR(LexEntry) *)vc_malloc((size_t)(uint32_t)t->count * sizeof(GPTR(LexEntry))));
        char *p = DLLVAR(char, 0x6371c5e8);     // word, pronunciation, word, ...
        for (int32_t i = 0; i < t->count; i++) {
            LexEntry *e = vc_malloc(sizeof(LexEntry));
            GPSET(GP(GPTR(LexEntry), t->entries)[i], e);
            GPSET(e->word, p);
            p += strlen(p) + 1;
            GPSET(GP(LexEntry, GP(GPTR(LexEntry), t->entries)[i])->pron, p);
            p += strlen(p) + 1;
        }
    }
    (*DLLVAR(int32_t, 0x63738518))++;
    vc_LeaveCriticalSection(CS);
}

void user_lex_file(GPTR(UserLex) *out) {
    vc_EnterCriticalSection(CS);
    if (*DLLVAR(int32_t, 0x63738514) == 0) {
        UserLex *t = vc_malloc(sizeof(UserLex));
        GPSET(*out, t);
        t->_0c = 0;
        t->count = 0;
        t->cap = 0;
        GPSET(t->entries, (GPTR(LexEntry) *)vc_malloc(0));
        user_lex_load(t);
    }
    (*DLLVAR(int32_t, 0x63738514))++;
    vc_LeaveCriticalSection(CS);
}

int32_t at_end(const char *s) { return *s == 0; }

void read_line(char *dst, int32_t n, char **src) {
    char *p = *src;
    char c = *p;
    while (c != '\r') {
        if (n-- == 0) break;
        *dst++ = c;
        c = *++p;
    }
    *p = 0;
    *dst = 0;
    *src = p + 2;
}

// a line of the data into buf (its '\n' dropped)
static void next_line(char *buf, char **data) {
    read_line(buf, 0x100, data);
    size_t l = strlen(buf);
    if (buf[l - 1] == '\n') buf[l - 1] = 0;
}

void words_set_f624(int32_t idx, int32_t v) { GP(WordInput, DLLPTR(WordInput, 0x63738968)[idx])->p.f624 = v; }

int32_t words_open(int32_t mode, char *data) {
    vc_EnterCriticalSection(CS);
    fe_once();
    int32_t idx = word_obj_create();
    (*DLLVAR(int32_t, 0x63738b78))++;
    vc_LeaveCriticalSection(CS);
    user_lex_builtin(DLLPTR(UserLex, 0x63738b68));
    user_lex_file(DLLPTR(UserLex, 0x63738b74));
    GPTR(WordInput) *slot = DLLPTR(WordInput, 0x63738968) + idx;
    WordInput *S = vc_malloc(sizeof(WordInput));
    GPSET(*slot, S);
    S = GP(WordInput, *slot);
    S->buf = 0;
    S->_04 = idx;
    S->rd = 0;
    S->wr = 0;
    S->len = 0;
    for (int k = 0; k < 9; k++) S->out[k] = 0;
    S->_38 = 0;
    S->flag = 0;
    S->marks = 0;
    S->p.mode = mode;
    vc_EnterCriticalSection(CS);
    word_set_rate(idx, 5);
    word_set_mode(idx, 3);
    word_set_events(idx, 0xf);
    vc_LeaveCriticalSection(CS);
    if (mode != 0) return idx;
    // the alternative prosody's data (mode 0): a table of up to 25 named rows, then records
    Prosody *P = &GP(WordInput, *slot)->p;
    P->nalt = 0;
    if (!data) return idx;
    char line[0x100];
    const char *sp = DLLVAR(const char, 0x6371da30), *eq = DLLVAR(const char, 0x6371da34);  // " ", "="
    int32_t n = 0;
    if (!at_end(data)) {
        for (;;) {
            if (n >= 0x19 || at_end(data)) break;
            next_line(line, &data);
            if (line[0] == '#') break;
            strcpy(P->alt_tab[n].name, vc_strtok(line, sp));
            for (int k = 0; k < 4; k++) {
                vc_strtok(NULL, eq);
                P->alt_tab[n].v[k] = vc_atoi(vc_strtok(NULL, eq));
            }
            n++;
            if (at_end(data)) break;
        }
    }
    P->nalt_tab = n;
    if (at_end(data)) return idx;
    for (;;) {
        uint8_t *r = vc_malloc(0x340);
        GPSET(P->alt[P->nalt], r);
        *(GPTR(char) *)(void *)(r + 0x14) = 0;
        *(int32_t *)(void *)(r + 0x18) = 0;
        if (at_end(data)) return idx;
        next_line(line, &data);
        *(int32_t *)(void *)r = vc_atoi(vc_strtok(line, sp));
        *(int32_t *)(void *)(r + 4) = vc_atoi(vc_strtok(NULL, sp));
        strcpy((char *)r + 8, vc_strtok(NULL, sp));
        char *tok = vc_strtok(NULL, sp);
        char *cp = vc_malloc(strlen(tok) + 1);
        GPSET(*(GPTR(char) *)(void *)(r + 0x14), cp);
        strcpy(cp, tok);
        if (at_end(data)) return idx;
        next_line(line, &data);
        int32_t *cnt = (int32_t *)(void *)(r + 0x18);
        for (char *t = vc_strtok(line, sp); t; t = vc_strtok(NULL, sp)) {
            ((int32_t *)(void *)(r + 0x1c))[*cnt] = vc_atoi(t);
            (*cnt)++;
        }
        if (at_end(data)) return idx;
        next_line(line, &data);
        int32_t k = 0;
        for (char *t = vc_strtok(line, sp); t; t = vc_strtok(NULL, sp)) ((int32_t *)(void *)(r + 0x1ac))[k++] = vc_atoi(t);
        P->nalt++;
        if (at_end(data)) return idx;
    }
}

void fe_entries_init(FrontEnd *fe) {
    fe->cap = 0x32;
    FeEntry *e = vc_malloc((size_t)(uint32_t)fe->cap * sizeof(FeEntry));
    fe->last = -1;
    GPSET(fe->entries, e);
    e[fe->cap - 1].pos = 2000000000;
}

FrontEnd *fe_ctor(FrontEnd *fe) {
    fe->queue = 0;
    fe->queue_in = 0;
    fe->_00 = 0;
    fe->_20 = 0;
    fe->words = 0;
    fe->in_tp = 0;
    vc_EnterCriticalSection(CS);
    if (*DLLVAR(int32_t, 0x63738bcc) == 0) *DLLVAR(int32_t, 0x63738bd0) = words_open(1, NULL);
    (*DLLVAR(int32_t, 0x63738bcc))++;
    vc_LeaveCriticalSection(CS);
    fe->_20 = (uint32_t)*DLLVAR(int32_t, 0x63738bd0);
    memset(fe->cs, 0, sizeof fe->cs);
    vc_InitializeCriticalSection(fe->cs);
    fe->abort_event = vc_CreateEventA(1, 0);
    fe->done_event = vc_CreateEventA(1, 0);
    fe_entries_init(fe);
    return fe;
}

int32_t fe_setup(FrontEnd *fe, uint8_t *queue_in, uint8_t *queue_out, const uint8_t *voice, int32_t voice_idx,
                 int32_t f50, int32_t unused, uint32_t hwnd, uint32_t msg, int32_t f8c) {
    (void)unused;
    GPSET(fe->queue_in, queue_in);
    GPSET(fe->queue, queue_out);
    int32_t pitch = *(const uint16_t *)(const void *)(voice + 0x8d8);
    fe->pitch_set = pitch;
    fe->pitch = pitch;
    fe->hwnd = hwnd;
    fe->msg = msg;
    fe->_8c = f8c;
    fe->words = words_open(1, NULL);
    words_set_f624(fe->words, fe->pitch);
    words_set_pitch(fe->words, fe->pitch);
    fe->next = 0;
    fe->voice = voice_idx;
    fe->text_last = 0;
    fe->text = 0;
    fe->flags = 0;
    fe->addr_mode = 0;
    fe->f50 = f50;
    if (f50 < 0) fe_voice_flags(fe, -f50);
    return 1;
}
