/*
 * The MSVC 4.2 C runtime functions the engine calls.
 *
 * The hook build binds these to the copies statically linked into
 * CGRM_EN.DLL (so memory from the DLL's heap is always freed by the DLL's
 * heap).  The standalone build implements them in src/port/msvcrt.c with the
 * exact MSVC behaviour the engine relies on ("C" locale ctype, qsort and
 * bsearch element order, atol overflow wrap-around).
 *
 * strlen/strcpy/strcat/memcpy/memset/strcmp were inlined by the original
 * compiler; decompiled code uses the standard functions for those (with
 * tv_strcmp where the -1/0/+1 result is used arithmetically).
 */
#ifndef TV_CRT_H
#define TV_CRT_H

#include <stddef.h>
#include "tv_common.h"

/* @0x1007e5a0 */
void *TV_CDECL tv_malloc(size_t n);
/* @0x1007e110 */
void TV_CDECL tv_free(void *p);
/* @0x1007e8d0 */
void *TV_CDECL tv_realloc(void *p, size_t n);
/* operator new / operator delete */
/* @0x1007d910 */
void *TV_CDECL tv_new(size_t n);
/* @0x1007d7a0 */
void TV_CDECL tv_delete(void *p);

/* @0x1007dbf0 */
int TV_CDECL tv_isalpha(int c);
/* @0x1007dc30 */
int TV_CDECL tv_islower(int c);
/* @0x1007dc60 */
int TV_CDECL tv_isdigit(int c);
/* @0x1007dcc0 */
int TV_CDECL tv_isalnum(int c);
/* @0x1007ec30 */
int TV_CDECL tv_tolower(int c);
/* @0x1007def0 */
long TV_CDECL tv_atol(const char *s);
/* @0x1007dfa0 */
int TV_CDECL tv_atoi(const char *s);

/* @0x1007d670 */
char *TV_CDECL tv_strstr(const char *s, const char *sub);
/* @0x1007db30 */
char *TV_CDECL tv_strchr(const char *s, int c);
/* @0x1007deb0 */
int TV_CDECL tv_strncmp(const char *a, const char *b, size_t n);
/* @0x1007dfb0 */
char *TV_CDECL tv_strncat(char *d, const char *s, size_t n);
/* @0x1007e7d0 */
char *TV_CDECL tv_strncpy(char *d, const char *s, size_t n);
/* @0x1007e0e0 */
char *TV_CDECL tv_strdup(const char *s);
/* @0x1007d7b0 */
char *TV_CDECL tv_strupr(char *s);
/* @0x1007ed90 */
char *TV_CDECL tv_strlwr(char *s);
/* @0x1007eaa0 */
void *TV_CDECL tv_memmove(void *d, const void *s, size_t n);
/* @0x1007dde0 */
int TV_CDECL tv_sprintf(char *buf, const char *fmt, ...);
/* @0x1007e780 */
int TV_CDECL tv_sscanf(const char *s, const char *fmt, ...);
/* @0x1007d920 */
void TV_CDECL tv_qsort(void *base, size_t n, size_t width,
                       int(TV_CDECL *cmp)(const void *, const void *));
/* @0x1007d6f0 */
void *TV_CDECL tv_bsearch(const void *key, const void *base, size_t n, size_t width,
                          int(TV_CDECL *cmp)(const void *, const void *));
/* @0x10088d80 */
char *TV_CDECL tv_itoa(int v, char *buf, int radix);

/* MSVC's _stricmp in the "C" locale: ASCII case folding, then a byte
 * compare.  Written out here so neither build depends on the host having it. */
static inline int tv_stricmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    int x, y;

    do {
        x = *p++;
        y = *q++;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
    } while (x != 0 && x == y);
    return x - y;
}

/* The compiler's inline strcmp: exactly -1, 0 or 1. */
static inline int tv_strcmp(const char *a, const char *b)
{
    int r = strcmp(a, b);
    return (r > 0) - (r < 0);
}

#endif
