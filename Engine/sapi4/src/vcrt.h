// C runtime and Win32 string semantics of msttssyn.dll's environment (see vcrt.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// MSVCRT C-locale ctype bits (_pctype)
#define _UPPER 0x1
#define _LOWER 0x2
#define _DIGIT 0x4
#define _SPACE 0x8
#define _PUNCT 0x10
#define _CONTROL 0x20
#define _BLANK 0x40
#define _HEX 0x80
#define _ALPHA_BIT 0x100
uint16_t vc_ctype(int ch);          // 0 outside 0..127 (MSVCRT's "C" locale)
static inline int vc_isalpha(int ch) { return vc_ctype(ch) & (_UPPER | _LOWER | _ALPHA_BIT); }
static inline int vc_isdigit(int ch) { return vc_ctype(ch) & _DIGIT; }
static inline int vc_isspace(int ch) { return vc_ctype(ch) & _SPACE; }
static inline int vc_isupper(int ch) { return vc_ctype(ch) & _UPPER; }
static inline int vc_islower(int ch) { return vc_ctype(ch) & _LOWER; }
static inline int vc_isalnum(int ch) { return vc_ctype(ch) & (_UPPER | _LOWER | _DIGIT | _ALPHA_BIT); }
int vc_tolower(int ch);             // ASCII only, as MSVCRT tolower in the C locale
int vc_toupper(int ch);

// comparisons return only the sign (-1, 0, 1), as the emulator's MSVCRT does
int vc_strcmp(const char *a, const char *b);
int vc_strncmp(const char *a, const char *b, size_t n);
int vc_stricmp(const char *a, const char *b);
int vc_strnicmp(const char *a, const char *b, size_t n);
int vc_stricmp_n(const char *a, const char *b, size_t n);   // raw difference, used by the above
int vc_memcmp(const void *a, const void *b, size_t n);
char *vc_strrev(char *s);           // _strrev
int32_t vc_atoi(const char *s);
// sscanf(s, "=%i", v) as the emulator's MSVCRT does it: 1 read, 0 not, -1 at the end of the text
int vc_scan_eq_int(const char *s, int32_t *v);
// strtok as MSVCRT's (its state is per thread there; here one per program)
char *vc_strtok(char *s, const char *delim);
// bsearch as MSVCRT's (which element of equal ones it finds depends on the halving)
const void *vc_bsearch(const void *key, const void *base, uint32_t num, uint32_t width,
                       int (*cmp)(const void *, const void *));
char *vc_lstrcpynA(char *d, const char *s, uint32_t n);   // KERNEL32 lstrcpynA
uint32_t vc_CharLowerBuffA(char *s, uint32_t n);        // USER32 over CP1252; returns n
uint32_t vc_CharUpperBuffA(char *s, uint32_t n);     // atoi as the emulator's MSVCRT (strtol, base 10, to 32 bits)

// Windows code page 1252 and USER32's character functions over it
extern const uint16_t cp1252_to_uni[256];
uint8_t uni_to_cp1252(uint16_t u);
int cp1252_isalpha(uint8_t ch);
int cp1252_isupper(uint8_t ch);
int cp1252_islower(uint8_t ch);
uint8_t cp1252_toupper(uint8_t ch);
uint8_t cp1252_tolower(uint8_t ch);
int vc_IsCharAlphaNumericA(uint8_t ch);   // USER32: a letter, a digit, or superscript 1 2 3
// lstrcmp(i)A / CompareStringA word sort (an approximation of Win32's, see vcrt.c); returns -1/0/1
int vc_word_cmp(const uint8_t *a, int na, const uint8_t *b, int nb, int ignore_case);
int vc_lstrcmpA(const char *a, const char *b);
int vc_lstrcmpiA(const char *a, const char *b);
void vc_CharLowerA_str(char *s);
int vc_MultiByteToWideChar_cp1252(const uint8_t *s, int n, uint16_t *d);
int vc_wcsicmp(const uint16_t *a, const uint16_t *b);   // ASCII-only case folding, sign result
