#include "fe_text.h"
#include "fe_small.h"
#include "vcrt.h"
#include "crt_vc.h"
#include <string.h>

#define REC 0xc9
#define TEXT(r) ((char *)(r) + 0x65)
// the spoken names of the characters, 30 bytes each (the originals index with a signed char in
// places, an unsigned one in others)
#define NAME(i) DLLVAR(const char, 0x6371f6b8 + 30 * (int32_t)(i))
#define S_(a) DLLVAR(const char, a)
#define SPACE S_(0x6371da30)

int txt_is_alpha(char c) {
    int8_t a = (int8_t)c;
    return (a >= 0x41 && a <= 0x5a) || (a >= 0x61 && a <= 0x7a);
}

int txt_approx(char c, char *out) {
    out[0] = 0;
    if (c != '~') return 0;
    const char *s = S_(0x637214b8);
    if (strlen(s) >= 0x64) return 0;
    strcpy(out, s);
    return 1;
}

int txt_split(char *pre, char *s, char *post, char **rest) {
    int32_t n = 0;
    if (!is_digit_char(*s))
        for (;;) {
            if (txt_is_alpha(*s)) break;
            pre[n++] = *s;
            if (*s == 0 || n >= 0x64) return 0;
            s++;
            if (is_digit_char(*s)) break;
        }
    pre[n] = 0;
    *rest = s;
    if (*s == 0) return 0;
    char *e = s + strlen(s) - 1;
    int32_t k = 0;
    if (!txt_is_alpha(*e))
        for (;;) {
            if (is_digit_char(*e)) break;
            post[k++] = *e;
            if (*s == 0 || k >= 0x64) return 0;
            e--;
            if (txt_is_alpha(*e)) break;
        }
    post[k] = 0;
    e[1] = 0;
    vc_strrev(post);
    return 1;
}

void txt_email_part(char *dst, const char *sep, const char *word) {
    if (strlen(word) == 1 && !is_digit_char(word[0])) strcat(dst, NAME((int8_t)word[0]));
    else strcat(dst, vc_strcmp(word, S_(0x63721554)) ? word : S_(0x63721548));
    static const uint32_t seps[][2] = { { 0x63721544, 0x6372153c }, { 0x6371c088, 0x63721534 }, { 0x6371ed54, 0x63721524 },
                                        { 0x63721520, 0x63721518 }, { 0x63721514, 0x6372150c }, { 0x63721508, 0x63721500 } };
    for (int k = 0; k < 6; k++)
        if (!vc_strcmp(sep, S_(seps[k][0]))) { strcat(dst, S_(seps[k][1])); return; }
    if (vc_strcmp(sep, S_(0x637214fc)) && vc_strcmp(sep, S_(0x637214f8)) && strlen(sep) == 1)
        strcat(dst, NAME((int8_t)dst[0]));      // (the original indexes with dst, not sep)
}

int txt_emoticon(uint8_t *rec) {
    char name[0x64], rest[0x64], out[0x64];
    char *text = TEXT(rec), *s = text;
    int32_t n1 = 0, n2 = 0, found = 0;
    char eyes = '*', mouth = '*';
    name[0] = 0;
    rest[0] = 0;
    for (; *s; s++) {
        char c = *s;
        if (eyes == '*') {
            if (c == ':' || c == ';') eyes = c;
            else { name[n1++] = c; if (n1 > 0x21) return 0; }
        } else if (mouth == '*') {
            if (c == '(' || c == ')') { mouth = c; found = 1; }
            else if (c != '-') { name[n1++] = c; if (n1 > 0x21) return 0; eyes = '*'; }
        } else {
            rest[n2++] = c;
            if (n2 > 0x21) return 0;
        }
    }
    if (!found) return 0;
    name[n1] = 0;
    out[0] = 0;
    rest[n2] = 0;
    strcat(out, name);
    strcat(out, S_(0x637214f4));
    if (eyes == ';') strcat(out, S_(0x637214e8));
    else if (eyes != ':') return 0;
    if (mouth == ')') strcat(out, S_(0x637214d8));
    else if (mouth == '(') strcat(out, S_(0x637214c8));
    else found = 0;
    strcat(out, S_(0x6371c5c8));
    strcat(out, rest);
    if (found) strcpy(text, out);
    return 1;
}

int txt_tilde(uint8_t *rec) {
    char word[0x64], buf[0x64];
    char *text = TEXT(rec), *s = text;
    word[0] = 0;
    if (*s == ' ') {
        char c;
        do { c = s[1]; s++; if (c == 0) return 0; } while (c == ' ');
    }
    if (!is_digit_char(s[1])) strcpy(word, NAME((int8_t)*s));
    else if (txt_approx(*s, word) && strlen(word) + strlen(text) + 5 >= 0x64) return 0;
    if (!word[0]) return 0;
    strcpy(buf, SPACE);
    strcat(buf, word);
    strcat(buf, SPACE);
    char *p = text;
    while (*p == '~' || *p == ' ') p++;
    strcat(buf, p);
    strcpy(text, buf);
    return 1;
}

int txt_normalize(uint8_t *rec) {
    char out[0xd0], name[0x198 - 0xd0], pre[0x64], post[0x64], word[0x3f0 - 0x328];
    char tail[4];
    char *text = TEXT(rec), *p = text;
    int32_t n = 0, changed = 0;
    tail[0] = 0;
    if (*p == ' ') {
        do { p++; if (*p == 0) return 0; } while (*p == ' ');
    }
    char *e = p + strlen(p) - 1;
    while (e > p && (*e == ' ' || *e == 0xd)) e--;
    if (e < p) return 0;
    uint8_t c1 = (uint8_t)p[1];
    if (e - p == 2 && p[0] == '"' && p[2] == '"' && ((c1 > 0x20 && c1 < 0x41) || c1 >= 0x7e)) {
        strcpy(name, NAME(c1));
        if (strlen(name) + 2 >= 0x64) return 0;
        strcat(name, SPACE);
        strcpy(text, SPACE);
        strcpy(text, name);
        return 1;
    }
    char *star = strchr(p, '*');
    if (*p == '~') return txt_tilde(rec);
    if (star) {
        if (strlen(text) >= 0x63) return 0;
        strcpy(word, text);
        char *rest;
        if (!txt_split(pre, word, post, &rest)) return 0;
        p = rest;
        int ok = 1;
        for (char *q = pre; *q; q++)
            if (*q != '*' && *q != ' ') ok = 0;
        char *w = p;
        for (; *p; p++)
            if (!txt_is_alpha(*p) && *p != '\'') ok = 0;
        if (!ok) return 0;
        strcpy(text, pre);
        strcat(text, SPACE);
        strcat(text, w);
        strcat(text, SPACE);
        strcat(text, post);
        return 1;
    }
    for (uint8_t c; (c = (uint8_t)*p) != 0; p++) {
        if ((c >= 0x80 || c == '\\' || c == '_') && c != 0xa0 && c != 0x91 && c != 0x92 && c != 0x93 && c != 0x94 &&
            c != 0xab && c != 0xbb) {
            char al = 0;
            name[0] = 0;
            if ((uint8_t)*p >= 0x80) { al = DLLVAR(const char, 0x6371f5b8)[(c & 0x7f) * 2]; name[0] = al; }
            if (al) {
                out[n++] = al;
                changed = 1;
            } else {
                strcpy(name, NAME((uint8_t)*p));
                out[n] = ' ';
                out[n + 1] = 0;
                n++;
                strcat(out, name);
                n += (int32_t)strlen(name);
                out[n] = ' ';
                n++;
                if (n >= 0x64) { changed = 0; break; }
                changed = 1;
            }
        } else {
            out[n++] = (char)c;
            char *q = &out[n - 1];
            if ((uint8_t)*q == 0xa0) { *q = ' '; changed = 1; }
            if ((uint8_t)*q == 0x91 || (uint8_t)*q == 0x92) { *q = '\''; changed = 1; }
            if ((uint8_t)*q == 0x93 || (uint8_t)*q == 0x94) { *q = '"'; changed = 1; }
            uint8_t c2 = (uint8_t)*q;
            if ((c2 >= 0x80 || c2 <= 0x1f) && c2 != 0x0a && c2 != 0x0d && c2 != 0x1b) { *q = ' '; changed = 1; }
            if (n > 0x64) { changed = 0; break; }
        }
    }
    if (!changed) return 0;
    out[n] = 0;
    if (tail[0]) strcat(out, tail);
    if (strlen(out) > 0x64) return 0;
    strcpy(text, out);
    return 1;
}

int txt_email(uint8_t *rec) {
    char buf[0x3f8 - 0x330], out[0x330 - 0x13c], pre[0x64], post[0x64], word[0x74 - 0x10];
    char *text = TEXT(rec);
    if (strlen(text) >= 0x63) return 0;
    strcpy(buf, text);
    char *b = buf;
    int32_t n = 0;
    while (!is_digit_char(*b) && !txt_is_alpha(*b)) {
        pre[n++] = *b;
        if (*b == 0 || n >= 0x64) return 0;
        b++;
    }
    pre[n] = 0;
    char *e = b + strlen(b) - 1;
    int32_t k = 0;
    while (!txt_is_alpha(*e) && !is_digit_char(*e)) {
        post[k++] = *e;
        if (*b == 0 || k >= 0x64) return 0;
        e--;
    }
    post[k] = 0;
    e[1] = 0;
    vc_strrev(post);
    int32_t dots = 0, other = 0, alpha = 0;
    char *last = NULL;
    if (*b) {
        for (char *s = b;;) {
            if (*s == '.') { dots++; last = s; }
            else if (!is_digit_char(*s) && !txt_is_alpha(*s)) other = 1;
            if (txt_is_alpha(*s)) alpha = 1;
            if (*s == ' ') { dots = 0; break; }
            s++;
            if (!*s) break;
        }
    }
    if (dots == 1 && last && (!other || !alpha)) {
        last++;
        alpha = 0;
        for (; *last; last++) {
            if (!txt_is_alpha(*last)) { dots = 0; break; }
            alpha++;
        }
        if (alpha == 3) other = 1;
        else alpha = 0;
    }
    if (!dots) return 0;
    if (!other && dots <= 1) return 0;
    if (!alpha && dots <= 1) return 0;
    word[0] = 0;
    out[0] = 0;
    strcat(out, SPACE);
    if (!vc_strcmp(pre, S_(0x637214fc))) strcpy(pre, S_(0x63721584));
    strcat(out, pre);
    int32_t wn = 0;
    const char *empty = S_(0x63738510);
    for (; *b; b++) {
        const char *sep = NULL;
        switch (*b) {
        case '-': sep = S_(0x6371c088); break;
        case '.':
            if (b[1] != ' ' && b[1] != '\n' && b[1] != '\r') sep = S_(0x63721544);
            else {
                txt_email_part(out, empty, word);
                strcat(out, S_(0x6371ed78));
                wn = 0;
                word[0] = 0;
                continue;
            }
            break;
        case '/': sep = S_(0x63721514); break;
        case ':': sep = S_(0x63721520); break;
        case '<': case '>': sep = S_(0x637214fc); break;
        case '@': sep = S_(0x63721508); break;
        case ' ': continue;
        default:
            word[wn++] = *b;
            word[wn] = 0;
            continue;
        }
        txt_email_part(out, sep, word);
        if (strlen(out) > 0x64) return 0;
        wn = 0;
        word[0] = 0;
    }
    txt_email_part(out, empty, word);
    char *tail = post;
    if (post[0] == '>') {
        if (strlen(post) == 1) strcpy(post, S_(0x63721570));
        else { strcat(out, S_(0x63721558)); tail = post + 1; }
    }
    strcat(out, tail);
    strcpy(text, out);
    return 1;
}

int txt_thousands(uint8_t *rec) {
    char buf[0x1f4 - 0x12c], pre[0x64], post[0x12c - 0xc8], num[0x64];
    char *text = TEXT(rec);
    if (strlen(text) >= 0x63) return 0;
    strcpy(buf, text);
    char *rest;
    if (!txt_split(pre, buf, post, &rest)) return 0;
    char *p = rest;
    int32_t n = 0;
    while (*p == ' ') p++;
    for (;;) {
        char c = *p;
        if (c == 0 || c == ' ' || c == '\n' || c == '\r') break;
        num[n++] = c;
        if (!is_digit_char(c)) return 0;
        p++;
    }
    num[n] = 0;
    int32_t v = vc_atoi(num);
    if (v >= 0x5d4 && v <= 0x833) return 0;
    if (strlen(num) != 4) return 0;
    num[6] = 0;
    num[4] = num[3];
    num[3] = num[2];
    num[2] = num[1];
    num[1] = ',';
    num[5] = ' ';
    strcpy(text, pre);
    strcat(text, num);
    strcat(text, post);
    return 1;
}

int txt_plus(uint8_t *rec) {
    char buf[0x1fc - 0x134], pre[0x134 - 0xd0], post[0xd0 - 0x6c], out[0x6c - 8];
    char *text = TEXT(rec);
    if (strlen(text) >= 0x63) return 0;
    strcpy(buf, text);
    char *rest;
    if (!txt_split(pre, buf, post, &rest)) return 0;
    char *p = rest;
    if (*p == ' ') {
        do { p++; if (*p == 0) return 0; } while (*p == ' ');
    }
    char *e = p + strlen(p);
    while (e > p && *e == ' ') e--;
    e[1] = 0;
    char *w = p;
    int ok = 1;
    if (!*p) ok = 0;
    else
        for (; *p; p++)
            if (!is_digit_char(*p) && !txt_is_alpha(*p) && *p != '+' && *p != '\n' && *p != '\r') ok = 0;
    if (!ok) return 0;
    int32_t n = 0;
    for (; *w; w++) {
        if (*w == '+') { out[n++] = ' '; out[n++] = '+'; out[n] = ' '; }
        else out[n] = *w;
        n++;
        if (n >= 0x64) { ok = 0; break; }
    }
    out[n] = 0;
    if (!ok) return 0;
    strcpy(text, pre);
    strcat(text, out);
    strcat(text, post);
    return 1;
}

int txt_tags(char **pp, uint8_t *recs, int32_t i) {
    char *s = *pp;
    while (*s == ' ') s++;
    if (*s != 0x1b) return 1;
    char *dst = (char *)recs + i * REC;
    for (;;) {
        char *start = s;
        if (*s != ' ')
            do s++; while (*s != ' ');
        size_t cnt = strlen(dst) + (size_t)(s - start);    // (the original copies this many: too many)
        memcpy(dst + strlen(dst), start, cnt);
        dst[cnt] = 0;
        do { strcat(dst, SPACE); s++; } while (*s == ' ');
        if (strlen(dst) > 0x64) return 1;
        if (*s != 0x1b) break;
    }
    *pp = s;
    return 0;
}

int txt_words(char **pp, uint8_t *recs, int32_t i) {
    char *s = *pp;
    char *text = TEXT(recs + i * REC);
    int32_t k = 0;
    for (char c = *s; c != 0x1b && c != 0; c = *s) {
        text[k++] = c;
        s++;
        if (k >= 0x64) return 1;
    }
    text[k] = 0;
    *pp = s;
    return 0;
}

int txt_rules(uint8_t *recs, int32_t i, int32_t spell) {
    uint8_t *rec = recs + i * REC;
    char *text = TEXT(rec);
    if (!text[0]) return 0;
    if (spell) {
        char name[0x64];
        strcpy(name, NAME((uint8_t)text[0]));
        strcpy(text, SPACE);
        strcat(text, name);
        strcat(text, SPACE);
        return 1;
    }
    return txt_emoticon(rec) || txt_normalize(rec) || txt_email(rec) || txt_thousands(rec) || txt_plus(rec);
}

int32_t txt_rewrite(char *text, GPTR(char) *out, int32_t spell) {
    uint32_t n = (uint32_t)strlen(text) >> 1;
    if (!n) return 0;
    uint8_t *recs = vc_malloc(n * REC);
    if (!recs) return 0;
    for (uint32_t k = 0; k < n; k++) { recs[k * REC] = 0; recs[k * REC + 0x64] = 0; recs[k * REC + 0x65] = 0; }
    char *p = text;
    int32_t count = 0, err = 0;
    if (*text) {
        for (;;) {
            if (txt_tags(&p, recs, count) || txt_words(&p, recs, count)) err = 1;
            if (err) goto done;
            if (!*p) break;
            count++;
        }
    }
    {
        uint32_t total = 0;
        int32_t changed = 0;
        for (int32_t k = 0; k <= count; k++) {
            if (txt_rules(recs, k, spell)) { recs[k * REC + 0x64] = 0; changed = 1; }
            total += (uint32_t)(strlen((char *)recs + k * REC) + strlen(TEXT(recs + k * REC)) + 1);
        }
        if (changed) {
            char *buf = vc_malloc(total);
            buf[0] = 0;
            for (int32_t k = 0; k <= count; k++) {
                strcat(buf, (char *)recs + k * REC);
                strcat(buf, TEXT(recs + k * REC));
            }
            GPSET(*out, buf);
        }
    }
done:
    vc_free(recs);
    return 0;
}
