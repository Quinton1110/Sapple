// The alternative prosody of the word module (prosody mode 0, used by characters such as "Excited"):
// the phrase's words become records keyed by their tape-7 characters; each word's start and end
// pitch take a random step around the value the voice's table gives its key; the phone pitch is those
// two points per word, interpolated with a cosine over the phones between.
#include "fe_phones.h"
#include "fe_reader.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

// MSVCRT _itoa, base 10 (the counts here are positive)
static char *itoa10(int32_t v, char *buf) {
    snprintf(buf, 0x14, "%d", (int)v);
    return buf;
}

LAYOUT(AltRec, f40, 0x40);
LAYOUT(AltRec, f60, 0x60);
LAYOUT(Prosody, w, 0x38);
LAYOUT(Prosody, f3d9c, 0x3d9c);
LAYOUT(Prosody, z3da8, 0x3da8);
LAYOUT(Prosody, marks, 0x3e3f);
LAYOUT(AltGroup, str, 0x14);
#ifdef DECOMP_HOOK
_Static_assert(sizeof(AltRec) == 0x68, "AltRec");
_Static_assert(sizeof(AltGroup) == 0x340, "AltGroup");
#endif

// word record i: the array has 0x97; the original also writes record 0x97, over the counters after it
static AltRec *W(Prosody *p, int32_t i) { return (AltRec *)(void *)((uint8_t *)p->w + (ptrdiff_t)0x68 * i); }

int32_t alt_class1(char c) {
    int32_t v = (signed char)c;
    return v == '1' || v == '7' || (v >= '3' && v <= '5') || v == 'A' || v == 'H' || v == 'a' || (v >= 'c' && v <= 'e') ||
           v == 'h' || v == 'p';
}

int32_t alt_class2(char c) {
    int32_t v = (signed char)c;
    return v == ')' || v == '2' || v == '8' || v == 'B' || v == 'F' || v == 'Y' || v == ']' || v == 'b';
}

int32_t alt_sonorant(char c) {
    const char *set = DLLVAR(const char, 0x6371ed08);
    const char *q = strchr(set, (signed char)c);
    if (!q) return 0;
    return q - set < 0x1c;
}

void pros_alt_a(Prosody *p) {
    const char *s1 = GP(const char, p->s[1]), *s3 = GP(const char, p->s[3]), *s4 = GP(const char, p->s[4]),
               *s5 = GP(const char, p->s[5]), *s7 = GP(const char, p->s[7]);
    int32_t c = -1, pos = 0, start;
    char c2 = '.', c3 = '~', cb = '~', c1 = ' ';
    int32_t pause = 0;      // (the original leaves this uninitialised until the first blank on tape 5)
    char *mk = p->marks;
    p->marks[0] = 0;
    p->nrecs = 0;
    if (s1[0] == '#')
        while (s1[++pos] == '#')
            ;
    start = pos;
    if (s1[pos] == 0) goto end;
    for (;;) {
        char ch = s1[pos];
        if (ch == '-' || ch == 0 || ch == '#') {        // a word ends here
            int32_t wl = 0, cl = 0;
            mk[0] = c2;
            mk[1] = 0;
            mk++;
            W(p, c + 1)->f44 = 0;
            c++;
            AltRec *R = W(p, c);
            if (pause == 1) {
                if (c < 0x97) W(p, c + 1)->f48 = 1;
                R->f4c = 1;
            } else {
                if (c < 0x97) W(p, c + 1)->f48 = 0;
                R->f4c = 0;
            }
            pause = 0;
            if (s1[pos] == '#') {
                R->f44 = 1;
                p->nrecs++;
                if (c < 0x97) W(p, c + 1)->f40 = 1;
            } else if (c < 0x97) {
                W(p, c + 1)->f40 = 0;
            }
            if (start == 1) R->f40 = 1;
            for (int32_t j = start; j < pos && wl < 0x1e; j++) {
                R->name[wl++] = s1[j];
                if (s7[j] != '~' && s7[j] != ' ') R->chars[cl++] = s7[j];
            }
            R->name[wl] = 0;
            R->chars[cl] = 0;
            if (alt_class1(R->chars[0]) == 1) {
                if (c > 0) W(p, c - 1)->f54 = 1;
                R->f50 = 1;
            } else {
                R->f50 = 0;
            }
            if (alt_class2(R->chars[0]) == 1) {
                if (c < 0x97) W(p, c + 1)->f50 = 1;
                R->f54 = 1;
            } else {
                if (c < 0x97) W(p, c + 1)->f50 = 0;
                R->f54 = 0;
            }
            if (cb == '~') cb = '_';
            R->c3e = cb;
            if (c1 == ' ') c1 = '_';
            R->c3d = c1;
            if (c3 == '~') c3 = '_';
            R->c3c = c3;
            start = pos + 1;
            c2 = '.';
            cb = '~';
            c1 = ' ';
        }
        if (s7[pos] != '~' && c2 == '.') c2 = s7[pos];
        if (s5[pos] != '~' && s5[pos] != ' ') c3 = s5[pos];
        if (s5[pos + 1] == ' ') pause = 1;
        if (s3[pos] != '~' && cb == '~') cb = s3[pos];
        if (s4[pos] != ' ' && c1 == ' ') c1 = s4[pos];
        if (s1[pos] == '#' && s1[pos + 1] == '#') break;
        pos++;
        if (s1[pos] == 0) break;
    }
end:
    mk[0] = c2;
    mk[1] = 0;
    p->f34 = c;
}

// the keys of words i.. up to the end of the phrase, runs of keyless words as counts
static void group_string(Prosody *p, int32_t *i, char *out, int32_t *n, int32_t *count) {
    char num[0x14];
    int32_t run = 0, k = *i;
    strcpy(out, DLLVAR(const char, 0x63738510));
    for (; k <= p->f34 && W(p, k)->f44 != 1; k++) {
        const char *key = W(p, k)->chars;
        (*count)++;
        if (key[0] && key[0] != '_') {
            (*n)++;
            if (run) {
                strcat(out, itoa10(run, num));
                strcat(out, DLLVAR(const char, 0x6371ed54));     // "_"
            }
            strcat(out, DLLVAR(const char, 0x6371ed68));         // "*"
            strcat(out, key);
            strcat(out, DLLVAR(const char, 0x6371ed54));
            run = 0;
        } else {
            run++;
        }
    }
    (*count)++;
    const char *key = W(p, k)->chars;
    if (key[0] && key[0] != '_') {
        (*n)++;
        if (run) {
            strcat(out, itoa10(run, num));
            strcat(out, DLLVAR(const char, 0x6371ed54));
        }
        strcat(out, DLLVAR(const char, 0x6371ed68));
        strcat(out, key);
        strcat(out, DLLVAR(const char, 0x6371ed54));
        run = 0;
    } else {
        run++;
    }
    if (run) strcat(out, itoa10(run, num));
    size_t l = strlen(out);
    if (out[l - 1] == '_') out[l - 1] = 0;     // (reads before the string when it is empty, as the original)
    *i = k;
}

void pros_alt_b(Prosody *p) {
    char str[0x7e8];
    static char last[0x14];     // (a stack buffer in the original: only written when the group ends a phrase)
    AltGroup *g = vc_malloc((size_t)(uint32_t)(p->nrecs + 1) * sizeof(AltGroup));
    GPSET(p->recs, g);
    for (int32_t i = 0; i <= p->nrecs; i++) g[i].str = 0;
    p->nrecs = 0;
    // (the original also builds a description of each word, "[key,marks,...]", in a local it never uses)
    for (int32_t i = 0; i <= p->f34; i++) {
        AltRec *R = W(p, i);
        if (strlen(R->chars) == 0) {
            R->chars[0] = '_';
            R->chars[1] = 0;
        }
        if (i == 0) W(p, 0)->f48 = 1;
        if (i == p->f34) R->f4c = 1;
        if (R->f40 == 1 || (R->f48 != 1 && R->f50 != 1)) {
            if (R->f40 == 1) {
                R->f48 = 1;
                R->f50 = 1;
                p->f3d94++;
                memset(p->z3da8, 0, sizeof p->z3da8);
                int32_t n = 0, count = 0, k = i;
                group_string(p, &k, str, &n, &count);
                if (W(p, k)->f44 == 1) {
                    strcpy(last, W(p, k)->chars);
                    if (last[0] == 0) strcpy(last, DLLVAR(const char, 0x6371ed54));
                }
                AltGroup *G = GP(AltGroup, p->recs) + p->nrecs;
                G->n = n;
                G->count = count;
                memcpy((uint8_t *)G + 8, last, strlen(last) + 1);   // (strcpy: a long key runs on into `str`)
                char *d = vc_malloc(strlen(str) + 1);
                GPSET(G->str, d);
                strcpy(d, str);
                p->nrecs++;
            }
        }
        if (R->f44 == 1 || (R->f4c != 1 && R->f54 != 1)) {
            if (R->f44 == 1) {
                R->f4c = 1;
                R->f54 = 1;
                p->f3d9c++;
            }
        }
    }
}

int32_t alt_tab_find(const char *key, const Prosody *p) {
    for (int32_t e = 0; e < p->nalt_tab; e++)
        if (strcmp(key, p->alt_tab[e].name) == 0) return e;
    return -1;
}

void alt_predict(Prosody *p, void *unused, int32_t unused2) {
    (void)unused; (void)unused2;
    int32_t k = 0;
    for (int32_t r = 0; r < p->nrecs; r++) {
        const AltGroup *G = GP(const AltGroup, p->recs) + r;
        if (G->count <= 0) continue;
        for (int32_t i = 0; i < G->count; i++, k++) {
            AltRec *R = W(p, k);
            char key[0x28];
            strcpy(key, R->chars);
            if (i >= G->count - 1) {
                if (strcmp(R->chars, DLLVAR(const char, 0x6371ed64)) == 0) strcpy(key, DLLVAR(const char, 0x6371ed60));        // "a" -> "a]"
                else if (strcmp(R->chars, DLLVAR(const char, 0x6371ed5c)) == 0) strcpy(key, DLLVAR(const char, 0x6371ed58));   // "h" -> "h]"
                else if (strcmp(R->chars, DLLVAR(const char, 0x6371ed54)) == 0 || strcmp(R->chars, DLLVAR(const char, 0x6371ed50)) == 0 ||
                         strcmp(R->chars, DLLVAR(const char, 0x6371ed4c)) == 0)
                    strcpy(key, DLLVAR(const char, 0x6371ed48));                                                           // "_", "P", "S" -> "_]"
            }
            int32_t dir;
            if (strcmp(key, DLLVAR(const char, 0x6371ed48)) == 0 || strcmp(key, DLLVAR(const char, 0x6371ed60)) == 0) dir = 0;
            else if (strcmp(key, DLLVAR(const char, 0x6371ed44)) == 0 || strcmp(key, DLLVAR(const char, 0x6371ed40)) == 0 ||
                     strcmp(key, DLLVAR(const char, 0x6371ed5c)) == 0 || strcmp(key, DLLVAR(const char, 0x6371ed3c)) == 0 ||
                     strcmp(key, DLLVAR(const char, 0x6371ed38)) == 0)
                dir = 1;        // "p2", "h2", "h", "8", "7"
            else dir = vc_rand() % 2;
            int32_t e = alt_tab_find(key, p);
            if (e == -1) e = 0;
            const int32_t *v = p->alt_tab[e].v;
            int32_t hi = v[0] + v[1], lo = v[0] - v[1];
            if (R->f60 > hi) R->f60 = hi;
            else if (R->f60 < lo) R->f60 = lo;
            hi = v[2] + v[3];
            lo = v[2] - v[3];
            if (R->f64 > hi) R->f64 = hi;
            else if (R->f64 < lo) R->f64 = lo;
            int32_t a = vc_rand() % v[1];
            a += x87_ftol((double)a * (double)0.7f);
            if (dir == 0) a = -a;
            int32_t b = vc_rand() % v[3];
            b += x87_ftol((double)b * (double)0.7f);
            if (dir == 0) b = -b;
            if (i >= G->count - 1) {
                R->f60 = a + v[0];
                R->f64 = b + v[2];
            } else {
                int32_t base = dir == 0 ? v[0] : v[2];
                R->f60 = a + base;
                R->f64 = base + b;
            }
            double scale = (double)p->pitch_hz / (double)p->f624;
            R->f60 = x87_ftol((double)R->f60 * scale);
            R->f64 = x87_ftol((double)R->f64 * scale);
        }
    }
}

double alt_cosine(double x0, double x, double x1, double y0, double y1) {
    double t = (x - x0) * (3.1415926 / (x1 - x0));
    double w = cos(t) - -1.0;
    return (w * y0 + (2.0 - w) * y1) * 0.5;
}

void alt_interpolate(float *v, int32_t n) {
    int32_t c = 0;
    while (c < n) {
        while (!x87_je(0.0, v[c]) && c < n) c++;
        int32_t e = c, prev = c - 1;
        while (x87_je(0.0, v[e]) && e < n) e++;
        if (prev + 1 < e) {
            double x1 = (double)e, x0 = (double)prev;
            for (int32_t k = prev + 1; k < e; k++)
                v[k] = (float)x87_ftol(alt_cosine(x0, (double)k, x1, (double)v[prev], (double)v[e]));
        }
        c = e;
    }
}

void pros_alt_c(Prosody *p) {
    char one[2];
    alt_predict(p, NULL, p->nrecs);
    char *ph = vc_malloc((size_t)(uint32_t)p->f34 * 8 + 8);
    GPSET(p->phones, ph);
    ph[0] = 0;
    float *pitch = vc_malloc((size_t)(uint32_t)(p->f34 + 1) * 32);
    GPSET(p->pitch, pitch);
    int32_t n = 0, lastson = 0;
    for (int32_t k = 0; k <= p->f34; k++) {
        const AltRec *R = W(p, k);
        if (R->f40 == 1) {
            strcat(ph, "#");
            pitch[n++] = 100.0f;
        }
        size_t len = strlen(R->name);
        if (len <= 1) {
            one[0] = R->name[0];
            one[1] = 0;
            strcat(ph, one);
            pitch[n++] = (float)(((double)R->f60 + (double)R->f64) * (double)0.5f);
            continue;
        }
        int32_t last = -1, first = -1;
        for (int32_t j = 0; (size_t)j < strlen(R->name); j++) {
            if (code_is_vowel(R->name[j]) && first == -1) first = j;
            if (alt_sonorant(R->name[j])) {
                if (last == -1) last = (size_t)j == strlen(R->name) - 1 ? j - 1 : j;
                lastson = j;
            }
        }
        if (lastson == last && last > 0) last--;
        if (last == -1) last = 0;
        if (first == -1) first = 0;
        for (int32_t j = 0; (size_t)j < strlen(R->name); j++) {
            one[0] = R->name[j];
            one[1] = 0;
            strcat(ph, one);
            char ch = R->name[j];
            if ((ch == 'd' && R->name[j + 1] == 'Z') || (ch == 't' && R->name[j + 1] == 'S')) {
                if (last == j) last++;
                if (lastson == j) lastson++;
                one[0] = R->name[++j];
                strcat(ph, one);
                pitch[n++] = 0.0f;
            }
            pitch[n] = 0.0f;
            if (k == p->f34 ? j == last : j == first) pitch[n] = (float)R->f60;
            else if ((size_t)j == strlen(R->name) - 1) pitch[n] = (float)R->f64;
            n++;
        }
    }
    int32_t i = n - 1;
    while (i > 0 && x87_je(pitch[i], 0.0)) i--;
    for (int32_t k = i; k < n; k++)
        if (k >= 0) pitch[k] = pitch[i];
    alt_interpolate(pitch, n);
    p->npitch = n;
}

double alt_hz(float v) {
    int32_t base = 0x5f - *DLLVAR(const int32_t, 0x63738b7c);
    return pow(2.0, ((double)v - (double)base) * 0.020833333333333332) * 50.0;
}

void pros_alt_d(GPTR(float) *tmp, Prosody *p) {
    const float *t = GP(const float, *tmp);
    float *pitch = GP(float, p->pitch);
    for (int32_t i = 0; i < p->npitch; i++)
        pitch[i] = (float)((alt_hz(pitch[i]) - (double)t[i] * (double)-2.0f) * (double)0.3333333432674408f);
}
