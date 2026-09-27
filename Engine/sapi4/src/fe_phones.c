#include "fe_phones.h"
#include "fe_output.h"
#include "fe_word.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT(idx) GP(WordInput, DLLPTR(WordInput, 0x63738968)[idx])
#define CS DLLVAR(uint8_t, 0x63738b90)

void tapes_copy(int32_t idx, GPTR(char) *a, GPTR(char) *b, GPTR(char) *c, GPTR(char) *d, GPTR(char) *e) {
    uint8_t *w = GP(uint8_t, DLLPTR(uint8_t, 0x63739e9c)[idx]);
    int32_t n = W16(w, 2) + 1;
    GPTR(char) *dst[5] = { a, b, c, d, e };
    static const uint32_t off[5] = { 0x585, 0xb05, 0x1085, 0x1605, 0x438e };
    for (int k = 0; k < 5; k++) GPSET(*dst[k], (char *)vc_malloc((size_t)(uint32_t)n));
    for (int k = 0; k < 5; k++) memcpy(GP(char, *dst[k]), w + off[k], (size_t)(uint32_t)(n - 1));
    for (int k = 0; k < 5; k++) GP(char, *dst[k])[n - 1] = 0;
}

int32_t phrase_init(Prosody *p, const char *s1, const char *s2, const char *s3, const char *s4, const char *s5,
                    const char *s6, const char *s7, const char *s8, const char *s0, int32_t nev,
                    const uint8_t *ev, int32_t *marks) {
    const char *src[9] = { s1, s2, s3, s4, s5, s6, s7, s8, s0 };
    int32_t failed = 0;
    for (int k = 0; k < 9; k++) GPSET(p->s[k], (char *)vc_malloc(strlen(src[k]) + 1));
    GPSET(p->phone_pos, (int32_t *)vc_malloc(strlen(s2) * 4 + 4));
    for (int k = 0; k < 9; k++) {
        if (p->s[k]) strcpy(GP(char, p->s[k]), src[k]);
        else failed = 1;
    }
    int32_t nm = 0, j = 0, q = 0;
    int32_t *pos = GP(int32_t, p->phone_pos);
    for (int32_t i = 0; i < nev; i++) {
        const uint8_t *e = ev + 0x18 * i;
        int32_t kind = *(const int32_t *)(const void *)e;
        if (kind == 8) {
            marks[2 * nm + 1] = *(const int32_t *)(const void *)(e + 0x14);
            marks[2 * nm] = *(const int32_t *)(const void *)(e + 0xc);
            nm++;
        } else if (kind == 4) {
            pos[j] = *(const int32_t *)(const void *)(e + 0xc);
            j++;
            q++;
            while (GP(char, p->s[1])[q] == '-') q++;
        }
    }
    for (; (size_t)j <= strlen(s2); j++) pos[j] = 0x713fb300;
    marks[2 * nm + 1] = -1;
    marks[2 * nm] = 0x77359400;
    p->g0 = *DLLVAR(int32_t, 0x63738b6c);
    p->g4 = *DLLVAR(int32_t, 0x63738b70);
    p->pitch = 0;
    p->phones = 0;
    p->f3d94 = 0;
    return failed;
}

// sscanf "%d" as the emulator's MSVCRT does it: 1 if read
static int scan_d(const char **s, int32_t *v) {
    const char *p = *s;
    while (vc_isspace((uint8_t)*p)) p++;
    if (!*p) return -1;
    char buf[1024];
    int n = 0;
    while (p[n] && n < 1023 && ((p[n] >= '0' && p[n] <= '9') || (n == 0 && (p[n] == '-' || p[n] == '+')))) {
        buf[n] = p[n];
        n++;
    }
    buf[n] = 0;
    if (!n) return 0;
    char *e;
    long long r = strtoll(buf, &e, 10);
    if (e == buf) return 0;
    *v = (int32_t)(uint32_t)(unsigned long long)r;
    *s = p + (e - buf);
    return 1;
}
// sscanf(s, "(%d,%d)", a, b)
static void scan_pair(const char *s, int32_t *a, int32_t *b) {
    if (*s != '(') return;
    s++;
    if (scan_d(&s, a) != 1) return;
    if (*s != ',') return;
    s++;
    scan_d(&s, b);
}

int32_t phrase_parse(const char *s, GPTR(float) *pitch, GPTR(char) *phones, Prosody *p) {
    int32_t nt = 0, m = 0, sum = 0;
    int32_t a = 0, b = 0;      // (sscanf's targets keep what they held; the string is always well formed)
    const char *q = s;
    if (*q != ']') {
        do {
            if (*q == '(') {
                scan_pair(q, &a, &b);
                p->targets[nt][0] = a;
                p->targets[nt][1] = b;
                nt++;
            }
            q++;
        } while (*q != ']');
    }
    p->targets[nt][0] = 0x7fffffff;
    p->targets[nt][1] = 0;
    for (q = s; (uint8_t)*q > 0x14; q++)     // (from the start again: the first phone is "/x[n")
        if (*q == '[') {
            p->phone_chars[m] = q[-1];
            int32_t v = vc_atoi(q + 1);
            sum += v;
            p->phone_dur[m] = v;
            p->phone_end[m] = sum;
            m++;
        }
    p->phone_chars[m] = 0;
    if (*phones) vc_free(GP(char, *phones));
    char *ph = vc_malloc(strlen(p->phone_chars) + 1);
    GPSET(*phones, ph);
    strcpy(ph, p->phone_chars);
    float *pv = vc_malloc((size_t)(uint32_t)m * 4);
    GPSET(p->pitch, pv);
    int32_t j = 0;
    for (int32_t i = 0; i < m; i++) {
        int32_t t = p->phone_end[i];
        while (p->targets[j + 1][0] < t) j++;
        float dx = (float)((double)p->targets[j + 1][0] - (double)p->targets[j][0]);
        double frac = ((double)t - (double)p->targets[j][0]) / (double)dx;
        double y = (double)p->targets[j][1] * (1.0 - frac) + (double)p->targets[j + 1][1] * frac;
        pv[i] = (float)y;
    }
    *pitch = p->pitch;
    return m;
}

double pitch_value(int32_t mode, int32_t base, double x) {
    if (!mode) return x;
    double e = (x - (double)(0x5f - base)) * 0.020833333333333332;
    return pow(2.0, e) * 50.0;
}

GPTR(const char) phone_name(uint8_t code) {
    GPTR(const char) *tab = DLLPTR(const char, 0x63738528);
    if (*DLLVAR(int32_t, 0x63738520) == 0) {
        const char *codes = DLLVAR(const char, 0x6371d7b8);
        const char *names = DLLVAR(const char, 0x6371d7e8);
        for (int i = 0; codes[i]; i++) GPSET(tab[(uint8_t)codes[i]], names + 4 * i);
        *DLLVAR(int32_t, 0x63738520) = 1;
    }
    return tab[code];
}

int32_t name_is_vowel(const char *name) { return *name && strchr(DLLVAR(const char, 0x6371d9ec), *name) ? 1 : 0; }  // "AEIOU"

int32_t code_is_vowel(char code) {
    const char *codes = DLLVAR(const char, 0x6371d7b8);
    const char *f = strchr(codes, code);
    if (!f) return 0;
    return f - codes < 0x11;
}

// strchr(set, c) as the original uses it: c == 0 finds the terminator
static int in_set(const char *set, char c) { return c == 0 || strchr(set, c) != NULL; }

static void put_phone(PhoneIn **rec, const char *name, const char *digit, double pitch) {
    strcpy((*rec)->name, name);
    if (digit) strcat((*rec)->name, digit);
    (*rec)->nstates = 1;
    (*rec)->dur = 1.0f;
    (*rec)->a[0] = 1.0f;
    (*rec)->b[0] = (float)pitch;
    (*rec)++;
}
static void put_name(PhoneIn **rec, uint32_t name) {
    strcpy((*rec)->name, DLLVAR(const char, name));
    (*rec)++;
}
#define S_HASH 0x6371c07c       // "#"
#define S_SIL 0x6371c010        // "SIL"

// the phones before a break get longer: back from the end of the list to the last vowel
static void lengthen(PhoneIn *start, int32_t count, float f) {
    int32_t flag = 0;
    for (int32_t k = count - 2; k >= 0; k--) {
        PhoneIn *r = start + k;
        if (name_is_vowel(r[1].name)) return;
        if (flag && r->name[0] == '#') return;
        if (!in_set(DLLVAR(const char, 0x6371da08), r->name[0]) && strcmp(DLLVAR(const char, S_SIL), r->name) != 0) {
            r->dur = f;
            flag = 1;
        }
    }
}

int32_t phone_list(int32_t idx, const char *text, const char *phones, const float *pitch, PhoneIn *out,
                   int32_t count_only, int32_t final, const char *s7, const char *s1) {
    (void)phones; (void)s7; (void)s1;
    WordInput *S = SLOT(idx);
    Prosody *P = &S->p;
    const int32_t *marks = GP(int32_t, S->marks);
    PhoneIn *rec = out;
    int32_t count = 0, pause = 0, j = 0, prev_r = 0, stress = 0, evi = 0;
    double cur = 110.0, nxt = 110.0;
    char *p = (char *)text;
    const char *one = DLLVAR(const char, 0x6371c094), *zero = DLLVAR(const char, 0x6371da14);
    while (*p) {
        uint8_t bl = (uint8_t)*p;
        uint8_t cl = (uint8_t)GP(char, P->phones)[j];
        if (cl == '#' || cl == '?') {
            if (cl == '?' && P->phone_dur[j] == 0x46) {
                j++;
                continue;
            }
            // a pause
            if (!pause) {
                if (!count_only) put_name(&rec, S_HASH);
                count++;
            }
            if (!count_only) {
                strcpy(rec->name, DLLVAR(const char, S_SIL));
                rec->dur = (float)((double)P->phone_dur[j] * (double)0.001f);
                rec++;
            }
            count++;
            if (!count_only) put_name(&rec, S_HASH);
            count++;
            pause = 1;
            j++;
            continue;
        }
        int adv = 1;
        if (bl == cl) {
            cur = pitch_value(P->mode, S->pitch, (double)pitch[j]);
            if (GP(char, P->phones)[j + 1]) nxt = pitch_value(P->mode, S->pitch, (double)pitch[j + 1]);
        } else if (bl == 'R') {
            cur = pitch_value(P->mode, S->pitch, (double)pitch[j]);
        } else if (bl == 't' && cl == 'd') {
            *p = 'R';
            bl = 'R';
            cur = pitch_value(P->mode, S->pitch, (double)pitch[j]);
        } else if ((bl == 0x91 && cl == '$') || (bl == '$' && cl == 'i')) {
            *p = (char)cl;
            bl = cl;
            cur = pitch_value(P->mode, S->pitch, (double)pitch[j]);
        } else adv = 0;
        if (adv) j++;
        switch (bl) {
        case '#': case '.': case ',': case ';': case '(': case ')': case ':': case '!':
            if (!pause) {
                if (!count_only) put_name(&rec, S_HASH);
                count++;
            }
            if (!count_only && strchr(DLLVAR(const char, 0x6371da0c), bl)) lengthen(out, count, 1.2f);  // ",;:)"
            break;
        case 0x16: case ' ': case '?':
            if (!pause) {
                if (!count_only) put_name(&rec, S_HASH);
                count++;
                pause = 1;
            }
            break;
        case '"': case '\'':
            stress = 1;
            break;
        case '~': case '`': case '%': case '-': case 0xe: case 0x10: case 'M': case 0x9d: case 0x11: case 0x19:
        case 'G':
            break;
        default:
            if (bl == 't' && p[1] == 'S') {
                cur = nxt;
                if (!count_only) put_phone(&rec, DLLVAR(const char, 0x6371da1c), NULL, nxt);   // "CH"
                count++;
                pause = 0;
                j++;
                p++;
                break;
            }
            if (bl == 'd' && p[1] == 'Z') {
                cur = nxt;
                if (!count_only) put_phone(&rec, DLLVAR(const char, 0x6371da18), NULL, nxt);   // "JH"
                count++;
                pause = 0;
                j++;
                p++;
                break;
            }
            if (bl == '$' && p[1] == 'r') {
                cur = nxt;
                if (!count_only) put_phone(&rec, DLLVAR(const char, 0x6371c558), stress ? one : zero, nxt);   // "ER"
                count++;
                pause = 0;
                prev_r = 1;
                stress = 0;
                break;
            }
            {
                GPTR(const char) gname = phone_name(bl);
                if (!gname) goto next;
                const char *name = GP(const char, gname);
                if (!(prev_r && name[0] == 'R') && name[0] != 0 && name[0] != ' ' && name[0] != '#') {
                    if (code_is_vowel((char)bl)) {
                        if (bl == '$') stress = 0;
                        if (!count_only) put_phone(&rec, name, stress ? one : zero, cur);
                        count++;
                        pause = 0;
                        stress = 0;
                    } else {
                        if (!count_only) put_phone(&rec, name, NULL, cur);
                        count++;
                        pause = 0;
                    }
                }
                prev_r = name[1] == 'R';
                char al;
                if (strcmp(name, DLLVAR(const char, S_SIL)) != 0 || *DLLVAR(char, 0x63738960) == '#') al = name[1];
                else al = '#';
                *DLLVAR(char, 0x63738960) = al;
                if (al != '#') strcpy(DLLVAR(char, 0x63738964), name);
            }
            break;
        }
        if (pause) {
            // index marks at or before this phone's position
            while (GP(int32_t, P->phone_pos)[j] >= marks[2 * evi]) {
                if (!count_only) {
                    sprintf(rec->name, DLLVAR(const char, 0x6371da04), marks[2 * evi + 1]);   // "\%d"
                    rec++;
                }
                count++;
                evi++;
            }
        }
    next:
        p++;
    }
    if (!pause) {
        if (!count_only) put_name(&rec, S_HASH);
        count++;
    }
    if (!final) {
        if (!count_only) {
            strcpy(rec->name, DLLVAR(const char, S_SIL));
            rec->dur = 0.36f;
            rec++;
        }
        count++;
    }
    if (!count_only) strcpy(rec->name, DLLVAR(const char, S_HASH));
    count++;
    if (!count_only) lengthen(out, count, 1.3f);
    return count;
}

int32_t words_phrase(int32_t idx) {
    WordInput *S = SLOT(idx);
    char *c = GP(char, S->rd) + S->len;
    char ch = *c;
    *c = 0;
    GP(char, S->rd)[S->len] = ch;
    vc_EnterCriticalSection(CS);
    fe_word_prepare(idx);
    S = SLOT(idx);
    GPSET(S->rd, GP(char, S->rd) + S->len);
    tapes_copy(idx, &S->out[1], &S->out[3], &S->out[4], &S->out[6], &S->out[7]);
    vc_free(GP(char, S->out[3]));
    vc_free(GP(char, S->out[6]));
    int32_t r = fe_word_format(idx, &S->out[0]);
    tapes_copy(idx, &S->out[2], &S->out[3], &S->out[5], &S->out[6], &S->out[8]);
    vc_LeaveCriticalSection(CS);
    if (r <= 0 || !S->out[0]) return 0;
    vc_EnterCriticalSection(CS);
    int32_t n = fe_word_event_count(idx);
    uint8_t *ev = vc_malloc((size_t)(uint32_t)(n * 24));
    GPSET(S->marks, (int32_t *)vc_malloc((size_t)(uint32_t)(n * 8)));
    fe_word_events(idx, (int32_t *)(void *)ev);
    vc_LeaveCriticalSection(CS);
    phrase_init(&S->p, GP(char, S->out[1]), GP(char, S->out[2]), GP(char, S->out[3]), GP(char, S->out[4]),
                GP(char, S->out[5]), GP(char, S->out[6]), GP(char, S->out[7]), GP(char, S->out[8]),
                GP(char, S->out[0]), n, ev, GP(int32_t, S->marks));
    if (S->p.mode == 0) {
        pros_alt_a(&S->p);
        pros_alt_b(&S->p);
    }
    if (S->p.mode == 0) {
        pros_alt_c(&S->p);
        if (S->p.f34 > 0x4e20) {
            float *tmp = vc_malloc((size_t)(uint32_t)S->p.npitch * 4);
            for (int32_t i = 0; i < S->p.npitch; i++) tmp[i] = GP(float, S->p.pitch)[i];
            vc_free(GP(float, S->p.pitch));
            phrase_parse(GP(char, S->p.s[8]), &S->p.pitch, &S->p.phones, &S->p);
            GPTR(float) t;
            GPSET(t, tmp);
            pros_alt_d(&t, &S->p);
            vc_free(tmp);
        }
    } else phrase_parse(GP(char, S->p.s[8]), &S->p.pitch, &S->p.phones, &S->p);
    vc_free(ev);
    return phone_list(idx, GP(char, S->p.s[0]), GP(char, S->p.phones), GP(float, S->p.pitch), NULL, 1, 0,
                      GP(char, S->p.s[7]), GP(char, S->p.s[1]));
}

void phrase_free(Prosody *p, int32_t mode) {
    for (int k = 0; k < 8; k++)
        if (p->s[k]) {
            vc_free(GP(char, p->s[k]));
            p->s[k] = 0;
        }
    if (p->phone_pos) {
        vc_free(GP(int32_t, p->phone_pos));
        p->phone_pos = 0;
    }
    if (p->s[8]) {
        vc_free(GP(char, p->s[8]));
        p->s[8] = 0;
    }
    if (p->pitch) {
        vc_free(GP(float, p->pitch));
        p->pitch = 0;
    }
    if (p->phones) {
        vc_free(GP(char, p->phones));
        p->phones = 0;
    }
    if (mode == 0) {
        for (int32_t i = 0; i < p->nrecs; i++) {
            GPTR(char) q = GP(AltGroup, p->recs)[i].str;
            if (q) vc_free(GP(char, q));
        }
        vc_free(GP(AltGroup, p->recs));
    }
}

int32_t phones_build(int32_t idx, PhoneIn *out, int32_t final, int32_t unused) {
    (void)unused;
    WordInput *S = SLOT(idx);
    int32_t r = phone_list(idx, GP(char, S->p.s[0]), GP(char, S->p.phones), GP(float, S->p.pitch), out, 0, final,
                           GP(char, S->p.s[7]), GP(char, S->p.s[1]));
    S = SLOT(idx);
    phrase_free(&S->p, S->p.mode);
    if (S->marks) {
        vc_free(GP(int32_t, S->marks));
        S->marks = 0;
    }
    return r;
}

void words_abort(int32_t idx) {
    WordInput *S = SLOT(idx);
    phrase_free(&S->p, S->p.mode);
    if (S->marks) {
        vc_free(GP(int32_t, S->marks));
        S->marks = 0;
    }
    if (S->out[0]) {
        vc_free(GP(char, S->out[0]));
        S->out[0] = 0;
    }
    for (int k = 1; k < 9; k++) vc_free(GP(char, S->out[k]));
}

// the phone list as text, the floats exactly (for comparing builds; not part of the engine)
void phones_print(FILE *f, const PhoneIn *ph, int32_t n) {
    fprintf(f, "phrase %d\n", n);
    for (int32_t i = 0; i < n; i++) {
        const PhoneIn *p = &ph[i];
        fprintf(f, "%-15.15s %a %d", p->name, (double)p->dur, p->nstates);
        for (int k = 0; k < 5; k++) fprintf(f, " %a", (double)p->a[k]);
        for (int k = 0; k < 5; k++) fprintf(f, " %a", (double)p->b[k]);
        for (int k = 0; k < 5; k++) fprintf(f, " %a", (double)p->c[k]);
        fprintf(f, "\n");
    }
}
