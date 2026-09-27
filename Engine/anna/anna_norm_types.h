/* Type records of the Anna text normalizer (anna_norm.c). anna_token.ti points at one of these; the first
 * int is always the item type. Layouts follow the engine's records (offsets in comments) so a later stage
 * can port 0x0D5E7E2D (number / time / phone prosody), which reads them. Records and word lists live until
 * the next anna_norm_sentence() call. */
#ifndef ANNA_NORM_TYPES_H
#define ANNA_NORM_TYPES_H

#include <stdint.h>

typedef uint16_t wc;

struct anna_vstate;

/* one engine word record (0x20): SPVSTATE*, text (UTF-16 span or static ASCII string), len, POS (+0x18) */
typedef struct {
    const struct anna_vstate *st; /* +0x00 */
    const wc *t;                  /* +0x04 text in the input buffer (or NULL) */
    const char *s;                /* static table text (when t == NULL); NULL + t NULL = no text */
    int len;                      /* +0x08 */
    int pos;                      /* +0x18 */
    int midword;                  /* static text followed by a space in the engine's table ("left" of
                                     "left parenthesis"), not by a NUL */
} anna_nword;

typedef struct {
    anna_nword *w;
    int n, cap;
} anna_wlist;

/* integer part (0x80, 0x0D5EF3D0): v[0] = groups of three after the first; v[1 + 4g .. 4 + 4g] flags of group g
 * {ones word, tens word, hundred, scale word}; v[0x19] ordinal; v[0x1a] read digit by digit; v[0x1b] digits
 * read; v[0x1c] digits in the first group (count % 3); v[0x1d] thousands separators present */
typedef struct {
    int v[0x1e];
    const wc *start, *end; /* +0x78, +0x7c */
} anna_intpart;

typedef struct { /* 0xc, 0x0D5EF6B9 */
    int kind;
    const wc *s; /* +4 */
    int n;       /* +8 */
} anna_decpart;

struct anna_numinfo;
typedef struct { /* 0x10, 0x0D5EF9C5 */
    int over;                       /* +0 1 = read "x over y" (set by the fraction word builder) */
    struct anna_numinfo *num, *den; /* +4, +8 */
    const wc *vulgar;               /* +0xc the 1/4 1/2 3/4 character, or NULL */
} anna_fracpart;

typedef struct anna_numinfo { /* 0x20, 0x0D5F0F10: types 0x1006..0x100c, 0x100e, 0x100f */
    int type;
    int neg;             /* +4 */
    anna_intpart *ip;    /* +8 */
    anna_decpart *dp;    /* +0xc */
    anna_fracpart *fr;   /* +0x10 */
    const wc *start, *end; /* +0x14, +0x18 */
    anna_wlist words;    /* +0x1c (moved out when the item's words are built) */
} anna_numinfo;

typedef struct { /* 0xc: 0x1014, a four digit number read as a year */
    int type;
    const wc *s;
    int len;
} anna_yearinfo;

typedef struct { /* 0x14: 0x101b hyphenated, 0x0D5EBFB7 */
    int type;
    const int *left, *right;
    const wc *ls, *rs;
} anna_hypheninfo;

typedef struct { /* 0xc: 0x1029 "-abc", 0x0D5EB48E */
    int type;
    const wc *s;
    int len;
} anna_dashinfo;

typedef struct { /* 0x18: 0x100d currency, 0x0D5F2BA9 */
    int type;
    anna_numinfo *num;   /* +4 */
    anna_numinfo *cents; /* +8 */
    int scale;           /* +0xc "$2 billion" */
    int n_before, n_after; /* +0x10, +0x14 special items (bookmarks...) met while looking ahead */
} anna_currinfo;

typedef struct { /* 0x10: 0x1018 clock time, 0x0D5F569D */
    int type;
    int ampm;    /* +4 */
    int hundred; /* +8 "hundred hours" */
    int minutes; /* +0xc */
} anna_timeinfo;

typedef struct {
    const wc *s;
    int n;
} anna_wspan;

typedef struct { /* 0x1027 phone number, 0x0D5F1BD8 */
    int type;
    anna_numinfo *country;
    anna_wspan *area;
    int is800, one;
    anna_wspan *groups;
    int ngroups;
} anna_phoneinfo;

typedef struct { /* 0x1015 numeric date / 0x1016 date with words */
    int type;
    int weekday;
    int month;
    const wc *day;
    int daylen;
    const wc *year;
    int yearlen;
} anna_dateinfo;

typedef struct { /* 0xc: 0x1017 decade, 0x0D5E1529 */
    int type;
    const wc *century; /* first two digits, or NULL */
    int decade;
} anna_decadeinfo;

typedef struct { /* 0x1019 duration h:mm:ss / m:ss, 0x0D5F617E */
    int type;
    anna_numinfo *hours, *minutes;
    const wc *seconds;
} anna_durinfo;

typedef struct { /* 0xc: 0x101e range, 0x101d time range, 0x1028 currency range */
    int type;
    const int *left, *right;
} anna_rangeinfo;

typedef struct { /* 0x1013 ZIP (ADDRESS context) */
    int type;
    const wc *zip;
    const wc *plus4;
} anna_zipinfo;

typedef struct { /* 8: 0x101c state + ZIP */
    int type;
    anna_zipinfo *zip;
} anna_addrinfo;

typedef struct { /* 8: 0x1003 / 0x1004 abbreviation */
    int type;
    int index; /* anna_norm_abbrev() index (the engine keeps a pointer to the table entry) */
} anna_abbrevinfo;

typedef struct { /* 8: 0x1010..0x1012 SAPI <context> number records: type + inner record */
    int type;
    const int *inner;
} anna_ctxinfo;

#endif
