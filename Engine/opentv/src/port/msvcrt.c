/*
 * The MSVC 4.2 C runtime functions the engine calls, for the standalone
 * build.
 *
 * The hook build reaches into the copies statically linked into CGRM_EN.DLL.
 * Here there is no DLL, so these are written out.  Most are the standard
 * functions under another name; the ones that are not are the character
 * classes and atol, which read the "C" locale table the original CRT built
 * into its data -- the same table, so the same answers for bytes over 0x7f
 * as the original gives.
 */
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "crt.h"

/*
 * The locale's character-class table.  The engine never calls setlocale, so
 * this stays what the CRT set it to at load: the "C" locale, where nothing
 * over 0x7f is a letter.  The pointer is two bytes into the table, so index
 * -1 (EOF) is a slot of its own.
 */
/* @0x1012ef40 */ extern const tv_ref g_ctype;

#define CT_UPPER 0x001
#define CT_LOWER 0x002
#define CT_DIGIT 0x004
#define CT_SPACE 0x008
#define CT_ALPHA 0x100

int TV_CDECL tv_isalpha(int c)
{
    return TV_REF(uint16_t, g_ctype)[c] & (CT_ALPHA | CT_LOWER | CT_UPPER);
}

int TV_CDECL tv_islower(int c)
{
    return TV_REF(uint16_t, g_ctype)[c] & CT_LOWER;
}

int TV_CDECL tv_isalnum(int c)
{
    return TV_REF(uint16_t, g_ctype)[c] & (CT_ALPHA | CT_LOWER | CT_UPPER | CT_DIGIT);
}

int TV_CDECL tv_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

char *TV_CDECL tv_strupr(char *s)
{
    char *p;

    for (p = s; *p != '\0'; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - ('a' - 'A'));
    return s;
}

/* Leading space, an optional sign, then digits, accumulated in 32 bits and
 * allowed to wrap. */
long TV_CDECL tv_atol(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    int32_t n = 0;
    unsigned char c, sign;

    while (TV_REF(uint16_t, g_ctype)[*p] & CT_SPACE)
        p++;
    c = *p++;
    sign = c;
    if (c == '-' || c == '+')
        c = *p++;
    while (TV_REF(uint16_t, g_ctype)[c] & CT_DIGIT) {
        n = n * 10 + (int32_t)c - '0';
        c = *p++;
    }
    return (sign == '-') ? -n : n;
}

void *TV_CDECL tv_malloc(size_t n)
{
    return malloc(n);
}

void TV_CDECL tv_free(void *p)
{
    free(p);
}

/* operator new / operator delete: this CRT's new returns NULL on failure
 * rather than throwing, and the engine checks for it. */
void *TV_CDECL tv_new(size_t n)
{
    return malloc(n);
}

void TV_CDECL tv_delete(void *p)
{
    free(p);
}

char *TV_CDECL tv_strstr(const char *s, const char *sub)
{
    return strstr((char *)s, sub);
}

char *TV_CDECL tv_strchr(const char *s, int c)
{
    return strchr((char *)s, c);
}

void TV_CDECL tv_qsort(void *base, size_t n, size_t width,
                       int(TV_CDECL *cmp)(const void *, const void *))
{
    qsort(base, n, width, cmp);
}

void *TV_CDECL tv_bsearch(const void *key, const void *base, size_t n,
                          size_t width,
                          int(TV_CDECL *cmp)(const void *, const void *))
{
    return bsearch(key, base, n, width, cmp);
}
