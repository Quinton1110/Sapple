/* zf1_util.c - string and word-list helpers for zf1.  Portable C99. */
#include "zf1_int.h"
#include <stdlib.h>
#include <string.h>

size_t zf_strlen(const zf_char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

zf_char *zf_strndup(const zf_char *s, size_t n)
{
    zf_char *r;
    if (!s) return NULL;
    r = (zf_char *)malloc((n + 1) * sizeof(zf_char));
    if (!r) return NULL;
    memcpy(r, s, n * sizeof(zf_char));
    r[n] = 0;
    return r;
}

zf_char *zf_strdup(const zf_char *s) { return s ? zf_strndup(s, zf_strlen(s)) : NULL; }

zf_char *zf_strdup_a(const char *a)
{
    size_t n, i;
    zf_char *r;
    if (!a) return NULL;
    n = strlen(a);
    r = (zf_char *)malloc((n + 1) * sizeof(zf_char));
    if (!r) return NULL;
    for (i = 0; i <= n; i++) r[i] = (unsigned char)a[i];
    return r;
}

int zf_strcmp(const zf_char *a, const zf_char *b)
{
    static const zf_char e = 0;
    if (!a) a = &e;
    if (!b) b = &e;
    while (*a && *a == *b) a++, b++;
    return (int)*a - (int)*b;
}

int zf_strcmp_a(const zf_char *a, const char *b)
{
    static const zf_char e = 0;
    if (!a) a = &e;
    if (!b) b = "";
    while (*a && *a == (unsigned char)*b) a++, b++;
    return (int)*a - (int)(unsigned char)*b;
}

void zf_setstr(zf_char **dst, const zf_char *src)
{
    zf_char *n = (src && *src) ? zf_strdup(src) : NULL;
    free(*dst);
    *dst = n;
}

void zf_setstr_a(zf_char **dst, const char *a)
{
    zf_char *n = (a && *a) ? zf_strdup_a(a) : NULL;
    free(*dst);
    *dst = n;
}

zf_char *zf_from_utf8(const char *s, int n, int *outlen)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t len = n < 0 ? strlen(s) : (size_t)n, i = 0, k = 0;
    zf_char *r = (zf_char *)malloc((len + 1) * sizeof(zf_char));
    if (!r) return NULL;
    while (i < len) {
        uint32_t c = p[i], cp;
        int extra = 0;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = 0xFFFD; }
        i++;
        while (extra-- > 0 && i < len && (p[i] & 0xC0) == 0x80) cp = (cp << 6) | (p[i++] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            r[k++] = (zf_char)(0xD800 + (cp >> 10));
            r[k++] = (zf_char)(0xDC00 + (cp & 0x3FF));
        } else {
            r[k++] = (zf_char)cp;
        }
    }
    r[k] = 0;
    if (outlen) *outlen = (int)k;
    return r;
}

char *zf_to_utf8(const zf_char *s, int n)
{
    size_t len = n < 0 ? zf_strlen(s) : (size_t)n, i, k = 0;
    char *r = (char *)malloc(len * 4 + 1);
    if (!r) return NULL;
    for (i = 0; i < len; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) r[k++] = (char)c;
        else if (c < 0x800) { r[k++] = (char)(0xC0 | (c >> 6)); r[k++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            r[k++] = (char)(0xE0 | (c >> 12)); r[k++] = (char)(0x80 | ((c >> 6) & 63)); r[k++] = (char)(0x80 | (c & 63));
        } else {
            r[k++] = (char)(0xF0 | (c >> 18)); r[k++] = (char)(0x80 | ((c >> 12) & 63));
            r[k++] = (char)(0x80 | ((c >> 6) & 63)); r[k++] = (char)(0x80 | (c & 63));
        }
    }
    r[k] = 0;
    return r;
}

zf_char zf_tolower(zf_char c)
{
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? (zf_char)(c + 32) : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (zf_char)(c + 32);
    if (c >= 0x100 && c <= 0x137) return (c == 0x130) ? c : (zf_char)(c | 1);   /* CharLowerW keeps U+0130 */
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? (zf_char)(c + 1) : c;
    if (c >= 0x14A && c <= 0x177) return (zf_char)(c | 1);
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? (zf_char)(c + 1) : c;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return (zf_char)(c + 32);
    if (c >= 0x386 && c <= 0x38F) {
        if (c == 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return (zf_char)(c + 37);
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return (zf_char)(c + 63);
    }
    if (c >= 0x410 && c <= 0x42F) return (zf_char)(c + 32);
    if (c >= 0x400 && c <= 0x40F) return (zf_char)(c + 80);
    if (c >= 0xFF21 && c <= 0xFF3A) return (zf_char)(c + 32);
    return c;
}

zf_char zf_toupper(zf_char c)
{
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? (zf_char)(c - 32) : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (zf_char)(c - 32);
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x137) return (c == 0x131) ? 0x49 : (zf_char)(c & ~1);
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c : (zf_char)(c - 1);
    if (c >= 0x14A && c <= 0x177) return (zf_char)(c & ~1);
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c : (zf_char)(c - 1);
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) return (zf_char)(c - 32);
    if (c == 0x3C2) return 0x3A3;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return (zf_char)(c - 37);
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return (zf_char)(c - 63);
    if (c >= 0x430 && c <= 0x44F) return (zf_char)(c - 32);
    if (c >= 0x450 && c <= 0x45F) return (zf_char)(c - 80);
    if (c >= 0xFF41 && c <= 0xFF5A) return (zf_char)(c - 32);
    return c;
}

/* ---------------- words ---------------- */
void zf_word_init(zf_word *w)
{
    memset(w, 0, sizeof *w);
    w->lang = 0x409;
    w->pos = 0xFFFF;
    w->tpos = 0xFFFF;
    w->i180 = 8;
    w->s1b0 = zf_strdup_a("none");
    w->s1d0 = zf_strdup_a("none");
    w->bi = 2;
    w->cur_pron = -1;
    w->fs.lang = 0x409;
    w->fs.pos = 0xFFFF;
}

void zf_word_free(zf_word *w)
{
    int i;
    free(w->text); free(w->regular); free(w->ci); free(w->prev_chars); free(w->next_chars);
    free(w->pron); free(w->ne_type); free(w->s1b0); free(w->s1d0); free(w->s1f0); free(w->s2f8);
    for (i = 0; i < w->nprons; i++) free(w->prons[i]);
    free(w->prons);
    memset(w, 0, sizeof *w);
}

void zf_word_copy(zf_word *d, const zf_word *s)
{
    int i;
    *d = *s;
    d->text = zf_strdup(s->text); d->regular = zf_strdup(s->regular); d->ci = zf_strdup(s->ci);
    d->prev_chars = zf_strdup(s->prev_chars); d->next_chars = zf_strdup(s->next_chars);
    d->pron = zf_strdup(s->pron); d->ne_type = zf_strdup(s->ne_type);
    d->s1b0 = zf_strdup(s->s1b0); d->s1d0 = zf_strdup(s->s1d0); d->s1f0 = zf_strdup(s->s1f0); d->s2f8 = zf_strdup(s->s2f8);
    d->prons = NULL;
    if (s->nprons) {
        d->prons = (zf_char **)malloc(sizeof(zf_char *) * (size_t)s->nprons);
        for (i = 0; i < s->nprons; i++) d->prons[i] = zf_strdup(s->prons[i]);
    }
    d->priv = NULL;
}

zf_word *zf_wl_insert(zf_wordlist *l, int at)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        zf_word *nw = (zf_word *)realloc(l->w, sizeof(zf_word) * (size_t)nc);
        if (!nw) return NULL;
        l->w = nw;
        l->cap = nc;
    }
    if (at < 0 || at > l->n) at = l->n;
    memmove(l->w + at + 1, l->w + at, sizeof(zf_word) * (size_t)(l->n - at));
    l->n++;
    zf_word_init(&l->w[at]);
    return &l->w[at];
}

zf_word *zf_wl_push(zf_wordlist *l) { return zf_wl_insert(l, l->n); }

void zf_wl_remove(zf_wordlist *l, int at)
{
    if (at < 0 || at >= l->n) return;
    zf_word_free(&l->w[at]);
    memmove(l->w + at, l->w + at + 1, sizeof(zf_word) * (size_t)(l->n - at - 1));
    l->n--;
}

void zf_wl_free(zf_wordlist *l)
{
    int i;
    for (i = 0; i < l->n; i++) zf_word_free(&l->w[i]);
    free(l->w);
    l->w = NULL;
    l->n = l->cap = 0;
}
