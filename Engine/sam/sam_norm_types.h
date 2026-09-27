/* Normalizer type records shared with the word builder (sam_front.c): the engine keeps a pointer
 * to them in every token node, and FUN_5ed45ac0 reads them to give numbers their prosody. */
#ifndef SAM_NORM_TYPES_H
#define SAM_NORM_TYPES_H

#include <stdint.h>

typedef uint16_t wc;

typedef struct {
    const wc *t;
    int len;
    const char *s;
    uint32_t fpos; /* forced part of speech (state +0x24) */
} nword;

typedef struct {
    nword *w;
    int n, cap;
} wlist;

typedef struct {
    int v[0x1e];
    const wc *start, *end;
} intpart;

typedef struct { /* FUN_5ed4f757 */
    int kind;
    const wc *s;
    int n;
} decpart;

struct numinfo;
typedef struct { /* FUN_5ed4fbb4 */
    int over;
    struct numinfo *num, *den;
    const wc *vulgar; /* 1/4, 1/2, 3/4 characters */
} fracpart;

typedef struct numinfo { /* 0x20 record of FUN_5ed50e1a */
    int type;
    int neg;
    intpart *ip;
    decpart *dp;
    fracpart *fr;
    const wc *start, *end;
    wlist words;
} numinfo;

typedef struct { /* 0x1014: a four digit number read as a year */
    int type;
    const wc *s;
    int len;
} yearinfo;

typedef struct { /* 0x101b hyphenated, FUN_5ed4d66f */
    int type;
    const int *left, *right;
    const wc *ls, *rs;
} hypheninfo;

typedef struct { /* 0x1029 "-abc", FUN_5ed4ce91 */
    int type;
    const wc *s;
    int len;
} dashinfo;

typedef struct { /* generic record: just a type */
    int type;
} typeinfo;

typedef struct {
    int type;
    numinfo *num, *cents;
    int scale;
} currinfo;

typedef struct {
    int type;
    int ampm, hundred, minutes;
} timeinfo;

typedef struct {
    const wc *s;
    int n;
} wspan;

typedef struct {
    int type;
    numinfo *country;
    wspan *area;
    int is800, one;
    wspan *groups;
    int ngroups;
} phoneinfo;

#endif
