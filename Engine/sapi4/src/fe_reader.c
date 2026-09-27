#include "fe_reader.h"
#include "fe_phrase.h"
#include "fe_word.h"
#include "fe_output.h"
#include "fe_small.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LAYOUT(WordInput, len, 0x10);
LAYOUT(WordInput, flag, 0x3c);
LAYOUT(WordInput, p, 0x48);
LAYOUT(Prosody, pitch, 0x4640);
LAYOUT(Prosody, phone_dur, 0x7910);
LAYOUT(Prosody, recs, 0xf614);
LAYOUT(Prosody, pitch_hz, 0xf620);
LAYOUT(Prosody, mode, 0xfb40);
LAYOUT(Prosody, nalt, 0xf610);
LAYOUT(Prosody, alt_tab, 0xf628);
LAYOUT(Prosody, nalt_tab, 0xfb3c);
LAYOUT(UserLex, cap, 8);
LAYOUT(UserLex, entries, 4);

// ------------------------------------------------------------------ the reader's state
#define G8(a) (*DLLVAR(uint8_t, a))
#define G16(a) (*DLLVAR(int16_t, a))
#define G32(a) (*DLLVAR(int32_t, a))
#define GPV(a) (*DLLPTR(char, a))              // a pointer global, as stored
#define HP(a) GP(char, GPV(a))                  // ... as a host pointer
#define SETP(a, p) GPSET(GPV(a), (p))

#define R_CUR 0x63738be0        // char *: the character being read
#define R_START 0x63738c4c      // char *: the text
#define R_END 0x63738c7c        // char *: its last character
#define R_BASE 0x63738bf0       // char *: where positions of index events count from ("\x1bRst")
#define R_MARK 0x63738c90       // char *: the start of the current phrase
#define R_WSTART 0x63738be4     // char *: the start of the current word
#define R_SENT 0x63738c74       // char *: a sentence end seen
#define R_OUT 0x63738c54        // char *: w + 0x585, the input tape
#define R_CNT 0x63738c3c        // uint8 *: w + 5, events per tape position
#define R_W 0x63738c68          // uint8 *: the word object
#define R_OUT2 0x63738c6c       // char *: w + 0xb05
#define R_LEN 0x63738c38        // int16: characters on the tape (written as a dword by rd_close_word)
#define R_STATE 0x63738c60      // int16
#define R_WLEN 0x63738c70       // int16: length of the word being read
#define R_WPOS 0x63738bec       // uint16: its position on the tape
#define R_WEIGHT 0x63738c88     // int16: the phrase's length so far (2 per letter, 16 per digit or symbol)
#define R_SLASH 0x63738c50      // uint8: inside "/.../" (phonemes)
#define R_ADDR 0x63738c40       // int32: inside an address
#define R_ADDR2 0x63738c94      // int32
#define R_SENTLEN 0x63738c84    // int16: the tape length at the sentence end
#define R_CAPS 0x63738bd8       // uint8
#define R_WORDS 0x63738bdc      // int32: words since the sentence end
#define R_PITCH 0x63738c98      // int32
#define R_LIM1 0x63738c58       // int16
#define R_LIM2 0x63738c5c       // int16
#define R_RET 0x63738c64        // int16: why the phrase ended
#define R_CH 0x63738c78         // uint8: the last character read
#define P_LONG 0x63738c44       // int32 (rd_phrase's state)
#define P_OVER 0x63738c8c
#define P_AT2 0x63738c48
#define P_AT1 0x63738be8
#define P_COMMA 0x63738c80

static jmp_buf rd_jb;           // (the original's is at 0x63738bf8)

#define HW GP(uint8_t, *DLLPTR(uint8_t, R_W))
#define OUT HP(R_OUT)
#define PUT(c) do { OUT[G16(R_LEN)] = (char)(c); G16(R_LEN)++; } while (0)
#define TAB16(off) DLLVAR(const int16_t, off)   // .rdata: { rate, pitch } per voice number (0x636a0304)

static int is_ws(uint8_t c) { return c == '\r' || c == '\n' || c == ' ' || c == '\t'; }
static int is_heavy(uint8_t c) {        // a digit or a symbol: 16 towards the phrase length
    if (vc_ctype(c) & _DIGIT) return 1;
    switch (c) {
    case '(': case ')': case '[': case ']': case '>': case '<': case '$': case '%': case '~': case '+':
    case '-': case '/': case '\\': case '{': case '}': case '*': case '_': return 1;
    }
    return 0;
}
static int is_stop(char c) { return c == '.' || c == '?' || c == '!' || c == ':' || c == ';'; }

// ------------------------------------------------------------------ events

static int32_t ev_append(uint8_t *w, EvNode *n) {
    EvLink l;
    GPSET(l, n);
    if (!WPTR(w, 0xb4ac)) {
        WPTR(w, 0xb4b0) = l;
        WPTR(w, 0xb4ac) = l;
    } else {
        GP(EvNode, WPTR(w, 0xb4b0))->next = l;
        WPTR(w, 0xb4b0) = l;
    }
    W32(w, 0xb4b4)++;
    return 0;
}
static void ev_set14(EvNode *n, int32_t v) {
    n->ch = (int16_t)v;
    n->_16 = (int16_t)((uint32_t)v >> 16);
}

int32_t ev_add_text(uint8_t *w, int32_t pos, int32_t len) {
    if (!(w[0xb4b8] & 1)) return -2;
    EvNode *n = ev_alloc();
    if (!n) return -1;
    n->kind = 1;
    n->a = pos;
    n->b = len;
    // (at the head of the list, not the tail)
    EvLink l;
    GPSET(l, n);
    if (!WPTR(w, 0xb4ac)) WPTR(w, 0xb4b0) = l;
    else n->next = WPTR(w, 0xb4ac);
    W32(w, 0xb4b4)++;
    WPTR(w, 0xb4ac) = l;
    return 0;
}
int32_t ev_add_word(uint8_t *w, int32_t pos, int32_t len) {
    if (!(w[0xb4b8] & 2)) return -2;
    EvNode *n = ev_alloc();
    if (!n) return -1;
    n->kind = 2;
    n->a = pos;
    n->b = len;
    return ev_append(w, n);
}
static int32_t ev_add3(uint8_t *w, int32_t mask, int32_t v, int32_t pos, int32_t x) {
    if (!(w[0xb4b8] & mask)) return -2;
    EvNode *n = ev_alloc();
    if (!n) return -1;
    n->kind = mask;
    n->a = pos;
    n->b = x;
    ev_set14(n, v);
    return ev_append(w, n);
}
int32_t ev_add_index(uint8_t *w, int32_t v, int32_t pos, int32_t x) { return ev_add3(w, 8, v, pos, x); }
int32_t ev_add_z(uint8_t *w, int32_t v, int32_t pos, int32_t x) { return ev_add3(w, 0x40, v, pos, x); }
int32_t ev_add_value(uint8_t *w, int32_t pos, uint16_t v) {
    if (v > 9) return -2;
    EvNode *n = ev_alloc();
    if (!n) return -1;
    n->kind = 0x10;
    n->a = pos;
    n->ch = (int16_t)v;
    return ev_append(w, n);
}
int32_t ev_add_mark(uint8_t *w, int32_t pos, int32_t v) {
    EvNode *n = ev_alloc();
    if (!n) return -1;
    n->kind = 0x20;
    n->a = pos;
    ev_set14(n, v);
    return ev_append(w, n);
}

static int16_t rate_of(int32_t voice, int32_t v) {
    return (int16_t)(((TAB16(0x636a0304)[2 * voice] - v + 5 + 8) << 5) / 8);
}

int32_t word_reset(uint8_t *w) {
    if (!w || (uint16_t)W16(w, 0xb4ba) != 0x9b39) return -1;
    w[0xb4bc] = 0;
    W16(w, 0xb4cc) = 5;
    W32(w, 0xb4c8) = 1;
    W16(w, 0xb4ce) = 5;
    W32(w, 0xb4d4) = 1;
    W32(w, 0xb4d0) = 2;
    int16_t r = (int16_t)(((TAB16(0x636a0308)[0] + 8) << 5) / 8);
    W16(w, 0xb588) = r;
    W16(w, 0xb58a) = r;
    W32(w, 0xb4c4) = 8;
    W16(w, 0x5810) = TAB16(0x636a030a)[0];
    ev_clear(w);
    W32(w, 0xb58c) = 0;
    W32(w, 0xb590) = 0;
    return 0;
}

int32_t word_voice(uint8_t *w, int32_t v) {
    if (v < 1 || v > 9) return -1;
    W32(w, 0xb4c8) = v;
    W16(w, 0xb4cc) = 5;
    W16(w, 0xb4ce) = 5;
    int16_t r = (int16_t)(((TAB16(0x636a0304)[2 * v] + 8) << 5) / 8);
    W16(w, 0xb588) = r;
    W16(w, 0xb58a) = r;
    W16(w, 0x5810) = TAB16(0x636a0304)[2 * v + 1];
    return 0;
}

// ------------------------------------------------------------------ reading

uint8_t rd_back(void) {
    char *p = HP(R_CUR);
    if (p > HP(R_START)) SETP(R_CUR, --p);
    return (uint8_t)*p;
}

static uint8_t rd_next(void) {
    if (GPV(R_CUR) == GPV(R_END)) longjmp(rd_jb, -1);
    SETP(R_CUR, HP(R_CUR) + 1);
    return (uint8_t)*HP(R_CUR);
}

void rd_init(char *text, int32_t len) {
    SETP(R_CUR, text);
    G16(R_LEN) = 0;
    G16(R_STATE) = 0;
    G16(R_WLEN) = 0;
    SETP(R_BASE, text);
    SETP(R_START, text);
    SETP(R_MARK, text);
    SETP(R_END, len > 0 ? text + len - 1 : text);
}

int32_t rd_phrase(uint8_t c) {
    if (c == 0) {
        G32(P_LONG) = G32(P_OVER) = G32(P_AT2) = G32(P_AT1) = 0;
        G16(R_RET) = 0;
        G32(P_COMMA) = 0;
        return 0;
    }
    if (G8(R_SLASH)) return 0;
    char *cur = HP(R_CUR);
    int32_t at = (int32_t)(cur - HP(R_START));
    if (G32(P_AT1)) {
        if (c == ',' || c == ';' || c == ':') G32(P_COMMA) = 1;
        else if (c == ' ' || c == '\t' || c == '\n') {
            if (G32(P_COMMA)) return G16(R_RET) = 1;
            G32(P_COMMA) = 0;
        } else G32(P_COMMA) = 0;
    } else G32(P_AT1) = at >= G16(R_LIM1);
    if (G32(P_AT2)) {
        if (c == '\t' || c == ' ' || c == '\n' || c == '\r') return G16(R_RET) = 2;
    } else G32(P_AT2) = at >= G16(R_LIM2);
    int16_t weight = G16(R_WEIGHT);
    if (G32(P_OVER)) {
        if (c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':') return G16(R_RET) = 3;
    } else if ((G32(P_OVER) = weight > 0x490) != 0) {
        G32(P_AT1) = 1;
        G32(P_AT2) = 1;
    }
    if (!G32(P_LONG)) {
        G32(P_LONG) = weight > 0x4a0;
        return 0;
    }
    G16(R_RET) = 4;
    if (GPV(R_WSTART) && G16(R_WPOS)) {
        if (cur > HP(R_WSTART))
            do rd_back();
            while (HP(R_CUR) > HP(R_WSTART));
        G16(R_LEN) = G16(R_WPOS);
        return 4;
    }
    G16(R_WEIGHT) = 0;
    G16(R_LEN) = 0;
    rd_phrase(0);
    return 0;
}

int32_t rd_close_word(int32_t start, int32_t *endp) {
    char *out = OUT;
    int32_t end = *endp;
    uint8_t c = (uint8_t)out[start];
    for (int32_t i = start; i < end; i++) G16(R_WEIGHT) -= is_heavy((uint8_t)out[i]) ? 0x10 : 2;
    int32_t j = start + 1;
    if (vc_ctype(c) & 0x107) {
        while (j < end && (uint8_t)out[j] == c) j++;
        if (j >= start + 10) {
            G16(R_LEN) = (int16_t)start;
            return 0;
        }
    } else {
        while (j < end && (uint8_t)out[j] == c) j++;
        if (j >= (c == '.' ? start + 10 : start + 3)) {
            G16(R_LEN) = (int16_t)start;
            G16(R_WLEN) = 0;
            return 0;
        }
    }
    // a run of one character: nothing to say for it. Otherwise, the word without its final
    // punctuation, looked up in the user lexicon
    out[end] = 0;
    char punct = 0, last = out[end - 1];
    if (last == '.' || last == '?' || last == '!' || last == ':' || last == ';' || last == ',') {
        out[end - 1] = 0;
        punct = last;
    }
    GPTR(char) rep = 0;
    if (user_lex_word(out + start, &rep)) {
        strcpy(out + start, GP(char, rep));
        free_cdecl(GP(char, rep));
        end = (int32_t)strlen(out + start) + start;
        if (punct) end++;
    } else if (punct == '.') {
        out[end - 1] = punct;
        punct = 0;
        if (user_lex_word(out + start, &rep)) {
            strcpy(out + start, GP(char, rep));
            free_cdecl(GP(char, rep));
            end = (int32_t)strlen(out + start) + start;
        }
    }
    if (punct) out[end - 1] = punct;
    int32_t one = 1;
    if (is_stop(out[end - 1])) {
        if (end - start <= 1 || !(vc_ctype((uint8_t)out[end - 2]) & _UPPER)) {
            G32(R_WORDS) = 0;
            SETP(R_SENT, HP(R_CUR) - 1);
        }
    }
    int32_t n = end - start;
    if (n > one && out[end - 1] == '"' && is_stop(out[end - 2])) {
        G32(R_WORDS) = 0;
        SETP(R_SENT, HP(R_CUR) - 1);
    }
    if (!(is_stop(out[end - 1]) && n == one)) {
        if (ev_add_word(HW, (int32_t)(HP(R_WSTART) - HP(R_BASE)), G16(R_WLEN)) == 0)
            GP(uint8_t, *DLLPTR(uint8_t, R_CNT))[(uint16_t)G16(R_WPOS)]++;
    }
    G16(R_SENTLEN) = (int16_t)end;
    for (int32_t i = start; i < end; i++) G16(R_WEIGHT) += is_heavy((uint8_t)out[i]) ? 0x10 : 2;
    *endp = end;
    G16(R_WLEN) = 0;
    return one;
}

// "\x1b..." argument words: up to 64 characters to a separator; NULL (and the reader put back 65
// characters) if longer
static char *rd_arg(char *buf) {
    int i = 0;
    for (;;) {
        uint8_t c = rd_next();
        buf[i] = (char)c;
        if (is_ws(c)) break;
        if (++i >= 0x40) break;
    }
    if (i == 0x40) {
        for (int k = 0; k < 0x41; k++) rd_back();
        return NULL;
    }
    buf[i] = 0;
    return buf;
}

// sscanf(s, "%lu %lu ...", ...) as the emulator's MSVCRT does it: the number of values read
static int scan_ulongs(const char *s, uint32_t **v, int n) {
    for (int k = 0; k < n; k++) {
        while (vc_isspace((uint8_t)*s)) s++;
        if (!*s) return k ? k : -1;
        char buf[1024];
        int m = 0;
        while (s[m] && m < 1023 && ((s[m] >= '0' && s[m] <= '9') || (m == 0 && (s[m] == '-' || s[m] == '+')))) {
            buf[m] = s[m];
            m++;
        }
        buf[m] = 0;
        if (!m) return k;
        char *e;
        long long r = strtoll(buf, &e, 10);
        if (e == buf) return k;
        s += e - buf;
        *v[k] = (uint32_t)(unsigned long long)r;
    }
    return n;
}

static void blank_brackets(char *buf) {
    char *p;
    if ((p = strchr(buf, '['))) *p = ' ';
    if ((p = strchr(buf, ','))) *p = ' ';
    if ((p = strchr(buf, ']'))) *p = ' ';
}

static void count_event(int32_t r) {
    if (r == 0) GP(uint8_t, *DLLPTR(uint8_t, R_CNT))[G16(R_LEN)]++;
}

static uint8_t rd_escape(void) {
    char *cur = HP(R_CUR);
    uint8_t bl = (uint8_t)*cur;
    char buf[0x44];
    if (bl == 0x1a) {
        G8(R_SLASH) = 0;
        G32(R_ADDR) = 0;
        if (G16(R_WEIGHT) != 0) return 0x1a;
        longjmp(rd_jb, -2);
    }
    if (bl != 0x1b) {
        if (is_ws(bl) || (bl >= 0xe && bl <= 0xaf)) return bl;
        return ' ';
    }
    char *esc = cur;
    uint8_t *w = HW;
    if (G16(R_LEN) == 0) SETP(R_MARK, cur);
    if (G16(R_WLEN) != 0) rd_close_word((uint16_t)G16(R_WPOS), DLLVAR(int32_t, R_LEN));
    uint8_t c = rd_next();
    switch (c) {
    case '"': case '#': case '\'': case '+': case '@':
        PUT(0x11);
        PUT(*HP(R_CUR));
        if (G16(R_STATE) == 0) G16(R_STATE) = 2;
        if (*HP(R_CUR) == '@') {
            G16(R_STATE)++;
            PUT(rd_next());
            PUT(' ');
        }
        break;
    case '[':
        if (!G8(R_SLASH)) {
            PUT(' ');
            PUT(0x11);
            int32_t at = G16(R_LEN);
            int16_t weight = G16(R_WEIGHT);
            PUT('[');
            while (G16(R_LEN) < 0x432) {
                uint8_t ch = rd_next();
                OUT[G16(R_LEN)] = (char)ch;
                char got = OUT[G16(R_LEN)];
                G16(R_LEN)++;
                if (got == ']') break;
                G16(R_WEIGHT)++;
            }
            OUT[G16(R_LEN)] = 0;
            if (strcmp(OUT + at, DLLVAR(const char, 0x63721c98)) == 0) {     // "[addr]"
                if (G32(R_ADDR) == 0) {
                    G32(R_ADDR) = 1;
                    PUT(' ');
                } else {
                    G16(R_WEIGHT) = weight;
                    G16(R_LEN) = (int16_t)(at - 2);
                }
            }
            if (strcmp(OUT + at, DLLVAR(const char, 0x63721ca8)) == 0) {     // "[\addr]"
                if (G32(R_ADDR) != 0) {
                    int16_t len = G16(R_LEN);
                    if (GPV(R_SENT)) {
                        G16(R_SENTLEN) = len;
                        SETP(R_SENT, HP(R_CUR));
                    }
                    G32(R_ADDR) = 0;
                    G32(R_ADDR2) = 0;
                    OUT[len] = ' ';
                    G16(R_LEN)++;
                } else {
                    G16(R_WEIGHT) = weight;
                    G16(R_LEN) = (int16_t)(at - 2);
                }
            }
            OUT[G16(R_LEN)] = ' ';
            break;
        }
        // fall through
    case '/':
        PUT(' ');
        PUT(0x11);
        PUT('/');
        if (G16(R_STATE) == 0) G16(R_STATE) = 2;
        G8(R_SLASH) = G8(R_SLASH) == 0;
        G32(R_ADDR2) = (G32(R_ADDR) != 0 && G8(R_SLASH)) ? 1 : 0;
        break;
    case 'N': case 'n': {
        if (!rd_arg(buf)) return rd_escape();
        uint32_t a = 0, b = 0, x = 0;
        blank_brackets(buf);
        uint32_t *v[3] = { &a, &b, &x };
        scan_ulongs(buf, v, 3);
        if (b == 0) b = (uint32_t)(esc - HP(R_BASE));
        count_event(ev_add_index(w, (int32_t)a, (int32_t)b, (int32_t)x));
        break;
    }
    case 'Z': case 'z': {
        if (!rd_arg(buf)) return rd_escape();
        uint32_t a = 0, b = 0, x = 0;
        blank_brackets(buf);
        uint32_t *v[3] = { &a, &b, &x };
        scan_ulongs(buf, v, 3);
        if (b == 0) b = (uint32_t)(esc - HP(R_BASE));
        count_event(ev_add_z(w, (int32_t)a, (int32_t)b, (int32_t)x));
        break;
    }
    case 'H': case 'h': {
        if (!rd_arg(buf)) return rd_escape();
        uint32_t a = 0;         // (uninitialised in the original: the escapes always carry a number)
        uint32_t *v[1] = { &a };
        scan_ulongs(buf, v, 1);
        count_event(ev_add_mark(w, (int32_t)(esc - HP(R_START)), (int32_t)a));
        return rd_escape();
    }
    case 'F': case 'f':
        if (G16(R_WEIGHT) != 0) {
            rd_back();
            rd_back();
            return 0x1a;
        }
        word_reset(HW);
        count_event(ev_add_value(HW, (int32_t)(esc - HP(R_START)), 8));
        break;
    case 'V': case 'v': {
        uint16_t v = (uint16_t)(rd_next() - 0x30);
        count_event(ev_add_value(HW, (int32_t)(esc - HP(R_START)), v));
        break;
    }
    case 'E': case 'e':
        rd_next();
        rd_next();
        return 0x1a;
    case 'C': case 'c':
        PUT(' ');
        SETP(R_SENT, (char *)NULL);
        G8(R_CAPS) = 1;
        break;
    case 'A': case 'a': {
        if (G16(R_WEIGHT) != 0) goto back_end;
        uint8_t ch = rd_next();
        if (ch == 'F' || ch == 'f') G32(R_PITCH) = 0;
        else if (ch == 'P' || ch == 'p') G32(R_PITCH) = 1;
        break;
    }
    case 'I': case 'i': {
        if (G16(R_WEIGHT) != 0) goto back_end;
        int32_t v = rd_next() - 0x30;
        if (v >= 1 && v <= 9) {
            W16(w, 0xb4ce) = (int16_t)v;
            W16(HW, 0x5810) = (int16_t)(TAB16(0x636a0306)[2 * W32(HW, 0xb4c8)] + (int16_t)(v * 4 - 0x14));
        }
        break;
    }
    case 'M': case 'm':
        if (G16(R_WEIGHT) != 0) goto back_end;
        switch (rd_next()) {
        case '0': W32(HW, 0xb4d4) = 2; break;
        case '1': W32(HW, 0xb4d4) = 0; break;
        case '2': W32(HW, 0xb4d4) = 1; break;
        case '3': W32(HW, 0xb4d4) = 3; break;
        }
        break;
    case 'P': case 'p':
        PUT(' ');
        PUT(0x11);
        PUT('P');
        PUT(rd_next());
        if (G16(R_STATE) == 0) G16(R_STATE) = 4;
        break;
    case 'Q': case 'q':
        longjmp(rd_jb, -1);
    case 'R': case 'r': {
        uint8_t ch = rd_next();
        if (ch == 's') {
            if (rd_next() == 't') {
                SETP(R_BASE, HP(R_CUR) - 3);
                break;
            }
            rd_back();
            rd_back();
            return rd_back();
        }
        if (G16(R_WEIGHT) != 0) {
            rd_back();
            goto back_end;
        }
        int32_t v = rd_escape() - 0x30;
        if (v >= 1 && v <= 9) {
            W16(HW, 0xb4cc) = (int16_t)v;
            int16_t r = rate_of(W32(HW, 0xb4c8), v);
            W16(HW, 0xb588) = r;
            W16(HW, 0xb58a) = r;
        }
        break;
    }
    case 'S': case 's':
        if (G16(R_WEIGHT) != 0) goto back_end;
        word_voice(HW, rd_next() - 0x30);
        break;
    case 'W': case 'w': {
        if (G16(R_WEIGHT) != 0) goto back_end;
        int32_t v = rd_next() - 0x30;
        if (v >= 0 && v <= 9) {
        } else if ((v >= 0x11 && v <= 0x16) || (v >= 0x31 && v <= 0x36)) {
            if (v >= 0x11 && v <= 0x16) v -= 7;
            if (v >= 0x31 && v <= 0x36) v -= 0x27;
        } else break;
        W32(HW, 0xb4d0) = v;
        break;
    }
    default:
        rd_next();
        return ' ';
    }
    rd_next();
    return rd_escape();
back_end:
    rd_back();
    return 0x1a;
}

static int32_t rd_word(void) {
    int32_t nl = 0;
    G16(R_WEIGHT) = 0;
    G8(R_CAPS) = 0;
    SETP(R_SENT, (char *)NULL);
    G16(R_SENTLEN) = 0;
    rd_phrase(0);
    uint8_t c = rd_escape();
    G8(R_CH) = c;
    if (c == 0x1a) goto end_text;
    for (;;) {
        if (nl) {
            if (c == '\r' || c == '\n') goto weight;
            if (c != ' ' && c != '\t') nl = 0;
        }
        if ((c == '\r' || c == '\n') && G16(R_WEIGHT) != 0) nl = 1;
        int32_t mode = W32(HW, 0xb4d4);
        if (mode == 3 && (c == '\r' || c == '\n') && G16(R_WEIGHT) != 0) goto weight;
        if (G8(R_SLASH) == 0) {
            if (mode == 1 && GPV(R_SENT) && G32(R_WORDS) == 0 && (vc_ctype(c) & _UPPER) && G16(R_WLEN) == 0) {
                if (G8(R_CAPS) == 0) {
                    // a capital after a sentence end: the sentence ends there
                    while (HP(R_CUR) > HP(R_SENT) + 1) rd_back();
                    G16(R_RET) = 0;
                    G16(R_LEN) = G16(R_SENTLEN);
                    goto weight;
                }
                G8(R_CAPS) = 0;
                SETP(R_SENT, (char *)NULL);
            }
            char *cur = HP(R_CUR);
            if (G16(R_STATE) == 0) {
                if (is_ws(c)) goto next;
                G16(R_STATE) = 1;
                SETP(R_MARK, cur);
                if (W32(HW, 0xb4d4) == 2) {
                    PUT(c);
                    rd_next();
                    G16(R_WEIGHT) = 2;
                    return 2;
                }
            } else G16(R_STATE)++;
            if (G16(R_WLEN) == 0) {
                if (!is_ws(c)) {
                    SETP(R_WSTART, cur);
                    G16(R_WLEN) = 1;
                    G16(R_WPOS) = G16(R_LEN);
                }
            } else if (!is_ws(c)) G16(R_WLEN)++;
            else {
                // the end of a word
                G32(R_WORDS)++;
                if (rd_close_word((uint16_t)G16(R_WPOS), DLLVAR(int32_t, R_LEN)) == 0) {
                    if (G16(R_WEIGHT) != 0) return G16(R_WEIGHT);
                    rd_phrase(0);
                    c = G8(R_CH);
                } else {
                    if (W32(HW, 0xb4d4) == 0) goto weight;
                    c = G8(R_CH);
                    if ((c == '\r' || c == '\n') && GPV(R_SENT) && G32(R_WORDS) == 0) goto weight;
                }
            }
            if (is_ws(c)) {
                c = ' ';
                G8(R_CH) = ' ';
            }
            G16(R_WEIGHT) += is_heavy(c) ? 0x10 : 2;
        } else {
            if (c != ' ' && is_ws(c)) goto next;
            G16(R_STATE)++;
            G16(R_WEIGHT)++;
            if (G16(R_WEIGHT) > 0x428 && !(vc_ctype(c) & _DIGIT) && c != '[' && c != ']' && c != '(' &&
                c != ')' && c != ',') {
                G16(R_RET) = 5;
                return G16(R_WEIGHT) + 0xa00;
            }
        }
        PUT(c);
        if (G8(R_SLASH) == 0) {
            int32_t r = rd_phrase(G8(R_CH));
            if (r != 0) {
                int16_t cx = G16(R_WEIGHT);
                if (r == 1) {
                    if (cx != 0) return cx;
                    return cx + 0x500;
                }
                if (r > 1 && r <= 4) return cx + 0x500;
            }
        }
    next:
        rd_next();
        c = rd_escape();
        G8(R_CH) = c;
        if (c == 0x1a) break;
    }
end_text:
    if (G16(R_WLEN) != 0) rd_close_word((uint16_t)G16(R_WPOS), DLLVAR(int32_t, R_LEN));
weight:
    return G16(R_WEIGHT);
}

static void add_addr_off(void) {
    if (G32(R_ADDR) == 0) return;
    const char *s = DLLVAR(const char, 0x63721ca8);     // "[\addr]"
    PUT(' ');
    PUT(0x11);
    strcpy(OUT + G16(R_LEN), s);
    G16(R_LEN) += (int16_t)strlen(s);
}

int32_t word_run(uint8_t *w, char *text, int32_t *len, int32_t flag) {
    if (!w || (uint16_t)W16(w, 0xb4ba) != 0x9b39) return -2;
    if (*len == 0) return -1;
    G32(R_ADDR) = W32(w, 0xb58c);
    G32(R_ADDR2) = W32(w, 0xb590);
    ev_clear(w);
    switch (setjmp(rd_jb)) {
    case 0: rd_init(text, *len); break;
    case -2: w[0xb4bc] = 0; return -3;
    case -1: return -1;
    }
    GPSET(*DLLPTR(uint8_t, R_W), w);
    memset(w + 5, 0, 0x500);
    SETP(R_OUT, (char *)w + 0x585);
    GPSET(*DLLPTR(uint8_t, R_CNT), w + 5);
    SETP(R_OUT2, (char *)w + 0xb05);
    if (G32(R_ADDR) != 0) {
        const char *s = DLLVAR(const char, 0x63721c98);     // "[addr]"
        int16_t n = (int16_t)strlen(s);
        PUT(' ');
        PUT(0x11);
        strcpy(OUT + G16(R_LEN), s);
        G16(R_LEN) += n;
        G16(R_WEIGHT) += (int16_t)(n + 2);
        PUT(' ');
        G16(R_WEIGHT)++;
    }
    if (flag == 0 && G32(R_ADDR2) == 0) G8(R_SLASH) = 0;
    else {
        PUT(' ');
        PUT(0x11);
        PUT('/');
        G8(R_SLASH) = 1;
        GPV(R_MARK) = GPV(R_CUR);
    }
    G16(R_LIM1) = W16(w, 0xb57a) > 0 ? W16(w, 0xb57a) : 0x500;
    G16(R_LIM2) = W16(w, 0xb57c) > 0 ? W16(w, 0xb57c) : 0x500;
    int32_t r;
    for (;;) {
        r = rd_word();
        *len = (int32_t)(HP(R_CUR) - HP(R_START));
        if (r < 0) {
            if (r == -5) return 7;
            if (r == -4) return -3;
            return r == -1 ? -1 : 0;
        }
        if (G16(R_STATE) != 0) break;
    }
    W16(w, 0) = 1;
    if (ev_add_text(w, (int32_t)(HP(R_MARK) - HP(R_START)), (int32_t)(HP(R_CUR) - HP(R_MARK))) == 0)
        (*GP(uint8_t, *DLLPTR(uint8_t, R_CNT)))++;
    W16(w, 0) = 1;
    W16(w, 2) = G16(R_LEN);
    w[0xb4bc] = 1;
    if (r > 0xa00) {
        add_addr_off();
        W32(w, 0xb584) = 0;
        return 5;
    }
    if (r > 0x500) {
        add_addr_off();
        W32(w, 0xb584) = 0;
    } else W32(w, 0xb584) = 1;
    return G16(R_RET);
}

int32_t word_dispatch(int32_t idx, char *text, int32_t *len, int32_t flag) {
    if (*DLLVAR(int32_t, 0x63738bd4) != (int32_t)0xffff9bad) return -0x66;
    if (idx < 1 || idx > 0x80) return -0x67;
    GPTR(uint8_t) wp = DLLPTR(uint8_t, 0x63739e9c)[idx];
    if (!wp) return -0x67;
    int32_t r = word_run(GP(uint8_t, wp), text, len, flag);
    static const int8_t code[9] = { 0x6f, 0x76, 0x6e, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75 };
    if ((uint32_t)(r + 3) > 8) return 0x76;
    return code[r + 3];
}

int32_t words_feed(int32_t idx, char *text, int32_t *consumed, int32_t *last) {
    GPTR(WordInput) *slot = DLLPTR(WordInput, 0x63738968) + idx;
    WordInput *in = GP(WordInput, *slot);
    if (in->buf) vc_free(GP(char, in->buf));
    char *b = vc_malloc(strlen(text) + 2);
    GPSET(in->buf, b);
    strcpy(b, text);
    in->rd = in->buf;
    in->wr = in->buf;
    in->len = 0;
    char *wr = GP(char, in->wr);
    size_t n = strlen(wr);
    wr[n] = 0x1a;
    wr[n + 1] = 0;
    GPSET(in->wr, wr + n + 1);
    in->len = (int32_t)(GP(char, in->wr) - GP(char, in->rd));
    vc_EnterCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    int32_t r = word_dispatch(idx, GP(char, in->rd), &in->len, in->flag);
    vc_LeaveCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    *consumed = (int32_t)(GP(char, in->rd) - GP(char, in->buf));
    *last = in->len + *consumed - 1;
    return r;
}

// ------------------------------------------------------------------ the user lexicon

uint8_t phone_code(const char *name) {
    const char *codes = DLLVAR(const char, 0x6371d7b8);
    const char *names = DLLVAR(const char, 0x6371d7e8);        // 4 bytes each
    for (int i = 0; codes[i]; i++)
        if (name[0] == names[4 * i] && strcmp(names + 4 * i, name) == 0) return (uint8_t)codes[i];
    return 0;
}

char *phones_to_codes(const char *pron) {
    char *b = vc_malloc(strlen(pron) + 1);
    strcpy(b, pron);
    size_t n = strlen(b);
    char *end = b + n;
    for (char *p = b; p <= end; p++)
        if (*p == ' ') *p = 0;
    char *out = vc_malloc(n + 10), *o = out;
    out[0] = 0;
    char *s = b;
    while (s < end) {
        if (*s == 0) {
            s++;
            continue;
        }
        size_t l = strlen(s);
        char last = s[l - 1];
        int32_t stress = 0;
        if (last <= '9' && last >= '0') {
            s[l - 1] = 0;
            stress = last - '0';
            if (stress == 1) *o++ = '\'';
            else if (stress == 2) *o++ = '`';
        }
        uint8_t c;
        if (stress == 0 && strcmp(s, DLLVAR(const char, 0x6371d9e8)) == 0) c = '$';     // "AH"
        else c = phone_code(s);
        if (c == 0) {
            vc_free(out);
            vc_free(b);
            return NULL;
        }
        if (c == 'X') {
            *o++ = 'd';
            *o = 'Z';
        } else if (c == 'Q') {
            *o++ = 't';
            *o = 'S';
        } else *o = (char)c;
        o++;
        if (c == 'e') *o++ = 'r';
        o[0] = 0;
        o[2] = 0;
        while (*s) s++;
    }
    vc_free(b);
    return out;
}

int lex_entry_cmp(const void *a, const void *b) {
    const char *k = GP(char, *GP(GPTR(char), *(const GPTR(GPTR(char)) *)a));
    const char *e = GP(char, GP(LexEntry, *(const GPTR(LexEntry) *)b)->word);
    return vc_stricmp(k, e);
}

static int key_cmp(const void *a, const void *b) {
    return vc_stricmp((const char *)a, GP(char, GP(LexEntry, *(const GPTR(LexEntry) *)b)->word));
}

GPTR(char) user_lex_find(const char *word, const UserLex *t) {
    char *k = vc_malloc(strlen(word) + 1);
    if (!k) return 0;
    strcpy(k, word);
    vc_EnterCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    GPTR(LexEntry) const *f = vc_bsearch(k, GP(GPTR(LexEntry), t->entries), (uint32_t)t->count,
                                         sizeof(GPTR(LexEntry)), key_cmp);
    vc_LeaveCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    vc_free(k);
    if (!f) return 0;
    return GP(LexEntry, *f)->pron;
}

static int word_char(char c) { return vc_isalpha(c) || strchr(DLLVAR(const char, 0x6371c574), c); }   // "-'."

int32_t user_lex_word(char *word, GPTR(char) *out) {
    char *s = word;
    while (*s && !word_char(*s)) s++;
    if (!*s) return 0;
    char *start = s;
    while (word_char(*s)) {
        if (!s[1]) {
            s++;
            break;
        }
        s++;
    }
    char *wend = s;
    while (*s && !word_char(*s)) s++;
    if (*s) return 0;
    size_t n = (size_t)(wend - start);
    char *key = vc_malloc(n + 1);
    memcpy(key, start, n);
    key[n] = 0;
    GPTR(char) pron = user_lex_find(key, GP(UserLex, *DLLPTR(UserLex, 0x63738b74)));
    if (!pron) pron = user_lex_find(key, GP(UserLex, *DLLPTR(UserLex, 0x63738b68)));
    vc_free(key);
    if (!pron) return 0;
    char *codes = phones_to_codes(GP(char, pron));
    if (!codes) return 0;
    size_t pre = (size_t)(start - word);
    char *res = vc_malloc(strlen(wend) + pre + strlen(codes) + 7);
    memcpy(res, word, pre);
    sprintf(res + pre, DLLVAR(const char, 0x6371da20), 0x10, codes, 0x10, wend);     // " %c %s %c %s"
    GPSET(*out, res);
    vc_free(codes);
    return 1;
}

// ------------------------------------------------------------------ continuing and settings

int32_t words_continue(int32_t idx, int32_t *consumed, int32_t *last) {
    GPTR(WordInput) *slot = DLLPTR(WordInput, 0x63738968) + idx;
    if (GP(WordInput, *slot)->out[0]) {
        vc_free(GP(char, GP(WordInput, *slot)->out[0]));
        GP(WordInput, *slot)->out[0] = 0;
    }
    for (int k = 1; k < 9; k++) vc_free(GP(char, GP(WordInput, *slot)->out[k]));
    WordInput *in = GP(WordInput, *slot);
    in->len = (int32_t)(GP(char, in->wr) - GP(char, in->rd));
    vc_EnterCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    int32_t r = word_dispatch(idx, GP(char, in->rd), &in->len, in->flag);
    vc_LeaveCriticalSection(DLLVAR(uint8_t, 0x63738b90));
    in = GP(WordInput, *slot);
    *consumed = (int32_t)(GP(char, in->rd) - GP(char, in->buf));
    *last = in->len + *consumed - 1;
    return r;
}

void words_set_pitch(int32_t idx, int32_t hz) {
    WordInput *in = GP(WordInput, DLLPTR(WordInput, 0x63738968)[idx]);
    in->p.pitch_hz = hz;
    double a = log((double)hz * 0.02) * 48.0;
    in->pitch = x87_ftol(a / log(2.0));
}
