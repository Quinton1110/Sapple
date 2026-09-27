/* zf1_ss.c - CSentSepImpl (rule-based sentence separator) of MSTTSEngine_OneCore.dll 10.3.21207.0.
 * Portable C99.  Clean-room re-implementation from the disassembly/decompilation (see notes/fe1_ss.md).
 *
 * Structure (engine addresses):
 *   18005d050  zf1_ss_run: skip leading blanks, run the state machine, 500-char cut
 *   180073fa0/180073a18/180073970  state init / reset / run loop (states -1..-7)
 *   1801623e0 (-1) 180064630 (-2) 180161a1c (-3) 180161ff8 (-4) 18016225c (-5) 180161e00 (-6) 1800686e0 (finish)
 *   18003da24  next token (keeps cur/prev/prev2 tokens, counters)   18003dbc0 the tokenizer
 *   helpers: 180163334 bracket stack, 180161040 quote counters, 180161120 abbreviation test, 18003f9e4 prefix
 *   lookup in a sorted word list, 180061864 exact lookup, 18003fbe4 emoticon shapes ...
 *
 * The engine's state object (~0x320 bytes) is emulated as an array of 32-bit words addressed by the engine's
 * byte offsets (W(0xa4) = *(uint32*)(state+0xa4)), so the transliteration stays checkable against the
 * decompilation.  Pointer members are only the text pointer (stored as an index-free flag: all token "base"
 * pointers point at the same text), the model and the char table (kept outside the word array).
 *
 * Token record (0x20 bytes at 0x98 = current, 0xb8 = previous, 0xd8 = the one before, 0x20 = tokenizer copy,
 * 0x58 = snapshot copy): +0 base ptr (8), +8 type, +0xc start, +0x10 len, +0x14 flags, +0x18 end flag.
 * Token types: 1 blank run, 2 private-use run (U+E800/E801), 3 word (letters/digits, URL/emoticon with flag
 * 0x41), 4 number ... (see notes), 8/9 ellipsis, 0xa open bracket, 0xb close bracket, 0xc sentence punctuation
 * (. ! ? ...), 0xd quote, 0xe , / : ;, 0xf other/NUL, 0x10 newline, 0x11 end of text, 0x12 emoticon shape,
 * 0x13 CJK full stop, 0x1a-0x20 symbol classes.
 */
#include "zf1_ss.h"
#include "zf1_ss_tab.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ character classes */
static uint32_t ucls(zf_char c) { return zf1_ucls_val[zf1_ucls_pages[zf1_ucls_pmap[c >> 8]][c & 0xff]]; }
static uint32_t ctb(zf_char c) { return zf1_ctype820[c & 0xff]; }
#define LO_(c) (((c) & 0xff00) == 0)
static uint32_t c_upper(zf_char c) { return LO_(c) ? (ctb(c) & 1) : (ucls(c) & 0x100000); }     /* 1801619e4 */
static uint32_t c_lower(zf_char c) { return LO_(c) ? (ctb(c) & 2) : (ucls(c) & 0x200000); }     /* 180066b20 */
static uint32_t c_digit(zf_char c) { return LO_(c) ? (ctb(c) & 4) : (ucls(c) & 0x400000); }     /* 18003f8fc */
static uint32_t c_alpha(zf_char c) { return LO_(c) ? (ctb(c) & 3) : (ucls(c) & 0x10000000); }   /* 18003f58c */
static uint32_t c_alnum(zf_char c) { return LO_(c) ? (ctb(c) & 7) : (ucls(c) & 0x10400000); }   /* 18003f9ac */
static uint32_t c_space(zf_char c) { return LO_(c) ? (ctb(c) & 8) : (ucls(c) & 0x800000); }
static int iswdigit_(zf_char c) { return (zf1_iswdigit_bits[c >> 3] >> (c & 7)) & 1; }
static int iswspace_(zf_char c) { return (zf1_iswspace_bits[c >> 3] >> (c & 7)) & 1; }
static zf_char c_tolower_ascii(zf_char c) { return (c >= 'A' && c <= 'Z') ? (zf_char)(c + 32) : c; }

/* 180072d30 */
static int ws_(zf_char c)
{
    return c_space(c) || (zf_char)(c - 1) <= 0x1e || (zf_char)(c - 0x7f) <= 0x20 || c == 0x202f || c == 0x205f ||
           c == 0x2060 || c == 0x2420 || c == 0x2422 || c == 0x2423 || c == 0x2424;
}
/* 180079ea4 */
static int nl_(zf_char c) { return c == 10 || c == 11 || c == 13 || c == 0x8d; }
/* 180160fe0 double-quote like */
static int dq_(zf_char c)
{
    zf_char d = (zf_char)(c - 0x93);
    return (d <= 0x28 && ((0x10001000003ULL >> (d & 0x3f)) & 1)) || c == 0x22 || (zf_char)(c - 0x201c) <= 3;
}
/* 180079148 */
static int rsq_(zf_char c) { return c == 0x27 || c == 0x92 || (zf_char)(c - 0x2019) <= 1 || c == 0x203a; }
/* 1800632f4 single-quote like */
static int sq_(zf_char c)
{
    zf_char d = (zf_char)(c - 0x2018);
    return c == 0x27 || c == 0x91 || (d <= 0x21 && ((0x200000009ULL >> (d & 0x3f)) & 1)) || rsq_(c);
}
/* 18007a664 */
static int quote_(zf_char c) { return sq_(c) || dq_(c); }
/* 180075e74 */
static int excl_(zf_char c)
{
    return c == 0x55e || c == 0x21 || c == 0x3f || c == 0x1c3 || c == 0x37e || c == 0x55c || c == 0x61f ||
           c == 0x203c || c == 0x2762 || c == 0xfe56 || c == 0xfe57;
}
/* 18003f2e4 */
static int cjkstop_(zf_char c) { return c == 0x589 || c == 0x964 || c == 0x3002 || c == 0xff01 || c == 0xff1f || c == 0xff61; }
/* 18016156c / 1801615c8 */
static int open_(zf_char c)
{
    return c == 0x300c || c == 0x28 || c == 0x5b || c == 0x7b || c == 0x2018 || c == 0x201c || c == 0x300e ||
           c == 0xff08 || c == 0xff3b || c == 0xff5b || c == 0xff62;
}
static int close_(zf_char c)
{
    return c == 0x300d || c == 0x29 || c == 0x5d || c == 0x7d || c == 0x2019 || c == 0x201d || c == 0x300f ||
           c == 0xff09 || c == 0xff3d || c == 0xff5d || c == 0xff63;
}
/* 18003f934 apostrophe-like */
static int apos_(zf_char c)
{
    return c == 0x27 || (zf_char)(c - 0x2018) <= 1 || c == 0xb4 || c == 0x60 || c == 0x2032 || c == 0xff07;
}
/* 18003f980 */
static int dot_(zf_char c) { return c == 0x2e || c == 0xff0e || c == 0x6d4; }
/* 18003f4f4 */
static int comma_(zf_char c) { return c == 0x2c || c == 0x2f || c == 0x3a || c == 0x3b; }
/* 18003f324 */
static int sym1_(zf_char c)
{
    return c == 0xb5 || c == 0x7c || c == 0x23 || c == 0x25 || c == 0x26 || c == 0x2a || c == 0x40 || c == 0x5c ||
           c == 0xa6 || c == 0xa7 || c == 0xa9 || c == 0xae || c == 0xb0 || c == 0x2103 || c == 0xb6 || c == 0xb7 ||
           c == 0x2020 || c == 0x2021 || c == 0x2030 || c == 0x2105 || c == 0x2106 || c == 0x2109 || c == 0x2116 ||
           c == 0x2122;
}
/* 18003f51c math */
static int math_(zf_char c)
{
    return c == 0x2211 || c == 0x2b || c == 0x3c || c == 0x3d || c == 0x3e || c == 0xac || c == 0xb1 || c == 0xd7 ||
           c == 0xf7 || c == 0x221a || c == 0x221e || c == 0x222b || c == 0x2248 || c == 0x2260 || c == 0x2264 ||
           c == 0x2265;
}
/* 18003f474 bullets */
static int bullet_(zf_char c)
{
    return c == 0x2022 || c == 0x25a1 || c == 0x25aa || c == 0x25ab || c == 0x25ca || c == 0x25cf || c == 0x25e6;
}
/* 18003f3cc currency */
static int curr_(zf_char c)
{
    return c == 0x20a4 || c == 0x9f3 || c == 0xe3f || c == 0x20a0 || c == 0x9f2 || c == 0x24 || c == 0xa2 ||
           c == 0x20ab || c == 0x20a6 || c == 0x20a7 || c == 0x20ac || c == 0xa3 || c == 0x20a8 || c == 0xa4 ||
           c == 0xa5 || c == 0x20a1 || c == 0x20a2 || c == 0x20a3 || c == 0x20ad || c == 0x20ae || c == 0x20af;
}
/* 18003f4b4 dashes */
static int dash_(zf_char c)
{
    return c == 0x2d || c == 0x2010 || c == 0x2012 || c == 0x2013 || c == 0x2014 || c == 0x2015 || c == 0x2212;
}
/* 18003f44c */
static int invq_(zf_char c) { return c == 0xbf || c == 0xa1; }
/* 1801619b4 */
static int lowdigit_(zf_char c) { return LO_(c) && (ctb(c) & 0x14) == 4; }
static int hi_(zf_char c) { return (zf_char)(c + 0x2800) < 0x400; }
static int lo_(zf_char c) { return (zf_char)(c + 0x2400) < 0x400; }

/* 180162b68 (case pair shift for chars >= 0x100), 180163210 tolower, 180163284 toupper */
static zf_char casemap(zf_char c, int16_t d, uint32_t want)
{
    uint32_t cl = ucls(c);
    zf_char hi = (zf_char)(c >> 8), r = c;
    int16_t s2;
    if (want & cl) return c;
    if (hi < 0x11) {
        if (hi == 0x10) { s2 = (int16_t)(d * 2); d = (int16_t)(d + s2); d = (int16_t)(d << 4); goto bef; }
        else if (hi == 1) {
            if (c < 0x19e) {
                if (c == 0x19d) { s2 = 0xd5; goto ef9; }
                else if (c < 400) {
                    if (c == 399) { s2 = 0xca; goto ef9; }
                    if (c == 0x130) return 0x130;
                    if (c == 0x131) return 0x131;
                    if (c == 0x178) { r = 0xff; goto bf2; }
                    if (c == 0x181) { s2 = 0xd2; goto ef9; }
                    if (c == 0x186) { s2 = 0xce; goto ef9; }
                    if (c != 0x189 && c != 0x18a) {
                        if (c == 0x18e) { s2 = (int16_t)(d * 0x4f); goto eff; }
                        goto bef;
                    }
                    s2 = 0xcd; goto ef9;
                }
                else if (c == 400) { s2 = 0xcb; goto ef9; }
                else {
                    if (c == 0x193) { s2 = 0xcd; goto ef9; }
                    if (c == 0x194) { s2 = 0xcf; goto ef9; }
                    if (c == 0x195) return 0x195;
                    if (c != 0x196) {
                        if (c == 0x197) { s2 = 0xd1; goto ef9; }
                        if (c != 0x19c) goto bef;
                    }
                    s2 = 0xd3; goto ef9;
                }
            }
            if (c < 0x1c7) {
                if (c != 0x1c6) {
                    if (c == 0x19e) return 0x19e;
                    if (c == 0x19f) { s2 = 0xd6; goto ef9; }
                    if (c == 0x1a9 || c == 0x1ae) { s2 = 0xda; goto ef9; }
                    if (c == 0x1b1 || c == 0x1b2) { s2 = 0xd9; goto ef9; }
                    if (c == 0x1b7) { s2 = 0xdb; goto ef9; }
                    if (c != 0x1c4) goto bef;
                }
            } else if (c != 0x1c7 && c != 0x1c9 && c != 0x1ca && c != 0x1cc) {
                if (c == 0x1dd) { s2 = (int16_t)(d * 0x4f); goto eff; }
                if (c != 0x1f1 && c != 499) goto bef;
            }
            d = (int16_t)(d * 2);
            goto bef;
        } else if (hi == 2) {
            if (0x217 < c) {
                if (c < 0x254) { s2 = 0xd2; goto ef9; }
                if (0x254 < c) {
                    if (599 < c) {
                        if (c < 0x259) goto bf2;
                        if (c < 0x25a) { s2 = 0xca; goto ef9; }
                        if (c < 0x25b) goto bf2;
                        if (c < 0x25c) { s2 = 0xcb; goto ef9; }
                        if (c < 0x25f) goto bf2;
                        if (0x260 < c) {
                            if (0x263 < c) {
                                if (c < 0x268) goto bf2;
                                if (c < 0x269) { s2 = 0xd1; goto ef9; }
                                if (0x269 < c) {
                                    if (c < 0x26c) goto bf2;
                                    if (0x26f < c) {
                                        if (c < 0x272) goto bf2;
                                        if (0x272 < c) {
                                            if (c < 0x274) goto bf2;
                                            if (0x275 < c) {
                                                if (c < 0x27f) goto bf2;
                                                if (0x283 < c) {
                                                    if (c < 0x287) goto bf2;
                                                    if (0x288 < c) {
                                                        if (0x28b < c) {
                                                            if (c < 0x291 || (0x292 < c && c < 0x2a9)) goto bf2;
                                                            s2 = 0xdb; goto ef9;
                                                        }
                                                        s2 = 0xd9; goto ef9;
                                                    }
                                                }
                                                s2 = 0xda; goto ef9;
                                            }
                                            s2 = 0xd6; goto ef9;
                                        }
                                        s2 = 0xd5; goto ef9;
                                    }
                                }
                                s2 = 0xd3; goto ef9;
                            }
                            s2 = 0xcf; goto ef9;
                        }
                    }
                    s2 = 0xcd; goto ef9;
                }
                s2 = 0xce; goto ef9;
            }
            goto bef;
        } else if (hi == 3) {
            if (0x3ef < c) return c;
            if ((c & 0xf0) == 0xd0) return c;
            if (c < 0x387) { s2 = (int16_t)(d * 0x26); goto eff; }
            if (c < 0x38b) { s2 = (int16_t)(d * 0x25); goto eff; }
            if (c < 0x38d) { d = (int16_t)(d << 6); goto bef; }
            if (c < 0x390) { s2 = (int16_t)(d * 0x3f); goto eff; }
            if (0x3ab < c) {
                if (c < 0x3ad) { s2 = (int16_t)(d * 0x26); goto eff; }
                if (c < 0x3b0) { s2 = (int16_t)(d * 0x25); goto eff; }
                if (0x3c1 < c) {
                    if (c < 0x3c3) { s2 = (int16_t)(d * 0x1f); goto eff; }
                    if (0x3cb < c) {
                        if (c < 0x3cd) { d = (int16_t)(d << 6); goto bef; }
                        if (0x3cf < c) goto bef;
                        s2 = (int16_t)(d * 0x3f); goto eff;
                    }
                }
            }
            d = (int16_t)(d << 5);
            goto bef;
        } else {
            if (hi != 4) {
                if (hi != 5) return c;
                s2 = (int16_t)(d * 2); d = (int16_t)(d + s2); d = (int16_t)(d << 4); goto bef;
            }
            if ((c & 0xf0) == 0 || (c & 0xf0) == 0x50) { s2 = (int16_t)(d << 2); d = (int16_t)(d + s2); d = (int16_t)(d << 4); goto bef; }
            if (c < 0x460) { d = (int16_t)(d << 5); goto bef; }
            goto bef;
        }
    } else {
        if (hi == 0x1e) goto bef;
        if (hi == 0x1f) {
            if (c < 0x1f70) goto l174;
            if (c < 0x1f72) { s2 = (int16_t)(d * 0x4a); goto l1b9; }
            if (c < 0x1f76) { s2 = (int16_t)(d * 0x56); goto l1b9; }
            if (c < 0x1f78) { s2 = (int16_t)(d * 100); goto l1b9; }
            if (c < 0x1f7a) { s2 = (int16_t)(d * 0x80); goto l1b9; }
            if (c < 0x1f7c) { s2 = (int16_t)(d * 0x70); goto l1b9; }
            if (0x1f7d < c) {
                if (c < 0x1fb0) goto bf2;
                if (0x1fb1 < c) {
                    if (c < 0x1fb5) goto bf2;
                    if (0x1fb9 < c) {
                        if (c < 0x1fbc) { s2 = (int16_t)(d * 0x4a); goto l1b9; }
                        if (c < 0x1fbd) goto bf2;
                        if (c < 0x1fcc) { s2 = (int16_t)(d * 0x56); goto l1b9; }
                        if (c < 0x1fcd) goto bf2;
                        if (0x1fd1 < c) {
                            if (c < 0x1fd4) goto bf2;
                            if (0x1fd9 < c) {
                                if (c < 0x1fdc) { s2 = (int16_t)(d * 100); goto l1b9; }
                                if (0x1fe1 < c) {
                                    if (c < 0x1fe5) goto bf2;
                                    if (0x1fe5 < c) {
                                        if (c < 0x1fea) goto l174;
                                        if (c < 0x1fec) { s2 = (int16_t)(d * 0x70); goto l1b9; }
                                        if (c != 0x1fec) {
                                            if (c < 0x1ffa) { s2 = (int16_t)(d * 0x80); goto l1b9; }
                                            s2 = (int16_t)(d * 0x7e); goto l1b9;
                                        }
                                    }
                                    s2 = (int16_t)(d * 7); goto l1b9;
                                }
                            }
                        }
                    }
                }
            l174:
                r = (zf_char)(c + d * 8);
                goto bf2;
            }
            s2 = (int16_t)(d * 0x7e);
        l1b9:
            r = (zf_char)(s2 + c);
            goto bf2;
        }
        if (hi == 0x21) { d = (int16_t)(d << 4); goto bef; }
        if (hi != 0x24) {
            if (hi != 0xff) return c;
            d = (int16_t)(d << 5);
            goto bef;
        }
        s2 = (int16_t)(d * 0x1a);
        goto eff;
    }
ef9:
    s2 = (int16_t)(d * s2);
eff:
    r = (zf_char)(c - s2);
    goto bf2;
bef:
    r = (zf_char)(c - d);
bf2:
    if (want & ucls(r)) c = r;
    return c;
}

static zf_char c_tolower(zf_char c)   /* 180163210 */
{
    if (LO_(c)) {
        if (ctb(c) & 1) {
            zf_char l = c_tolower_ascii(c);
            if (ctb(l) & 2) return l;
        }
        return c;
    }
    if ((ucls(c) >> 0x14) & 1) return casemap(c, -1, 0x200000);
    return c;
}

static zf_char c_toupper(zf_char c)   /* 180163284 */
{
    if (LO_(c)) {
        if (ctb(c) & 2) {
            zf_char u;
            if (c == 0xff) return 0x178;
            u = (c >= 'a' && c <= 'z') ? (zf_char)(c - 32) : c;
            if (ctb(u) & 1) return u;
        }
        return c;
    }
    if ((ucls(c) >> 0x15) & 1) return casemap(c, 1, 0x100000);
    return c;
}

/* ------------------------------------------------------------------ word lists */
/* model vtable slot -> list index in file order (CSentSepInfo vt 0x00..0x58) */
enum { L_CAPSTART = 4, L_EMPTY5 = 5, L_CONJ = 2, L_DIGITW = 6, L_ABBR = 0, L_BIBLE = 1, L_AMBIG = 3, L_NUMPFX = 7,
       L_TITLE = 10, L_SYMB = 8, L_SUFFIX = 11, L_EMOTI = 9 };

/* 180061864: exact lookup of s[0..n) in a sorted list -> index or -1 */
static int list_find(const zf1_sslist *l, const zf_char *s, uint32_t n)
{
    int lo = 0, hi = l->n - 1;
    while (lo <= hi) {
        int mid = (hi + lo) / 2;
        const zf_char *e = l->s[mid];
        uint32_t k;
        for (k = 0; k < n; k++) {
            if (e[k] == 0) goto greater;
            if (s[k] < e[k]) goto less;
            if (s[k] != e[k]) goto greater;
        }
        if (e[k] == 0) return mid;
    less:
        hi = mid - 1;
        continue;
    greater:
        lo = mid + 1;
    }
    return -1;
}

/* 18003f9e4: longest prefix of p[0..n) (at least minlen chars, followed by a non-alnum/non-'_' char) found in
 * list l; with l2 an optional "(suffix)" list.  Returns the matched length or 0. */
static uint32_t prefix_find(const zf_char *p, uint32_t n, uint32_t minlen, const zf1_sslist *l, const zf1_sslist *l2)
{
    int i12, i13, i14, i15, i3 = 0;
    uint32_t u4, u6, u7, u10;
    int li;
    if (!l) return 0;
    i14 = l->n - 1;
    i12 = 0;
    do {
        i13 = 0;
        i15 = i14;
        i14 = -1;
        while (u6 = 0, i12 <= i15) {
            const zf_char *e;
            uint32_t k;
            i3 = (i15 + i12) / 2;
            li = i3;
            e = l->s[li];
            u10 = 0;
            for (;;) {
                u4 = u10;
                if (n <= u4) break;
                if (e[u4] == 0) goto a83;
                {
                    zf_char a = p[u4], b = e[u4];
                    if (a > b) {
                        if (minlen <= u4 && i14 == -1 && a != 0x5f && c_alnum(a) == 0) {
                            i14 = i3 - 1;
                            i13 = i12;
                        }
                        goto b44;
                    }
                    if (a < b) goto b4c;
                }
                u10 = u4 + 1;
            }
            (void)k;
            if (u4 == n) {
                if (e[u4] == 0) return u10;
            b4c:
                i15 = i3 - 1;
            } else {
            a83:
                if (minlen <= u4) {
                    /* LAB_18003fa99 loop over following list entries */
                    for (;;) {
                        u7 = u6;
                        if (i15 < li) goto b6f;
                        if (p[u10] != 0x5f && c_alnum(p[u10]) == 0) u6 = u10;
                        u10 = 0;
                        u7 = u6;
                        if (li == i15) goto b6f;
                        li++;
                        e = l->s[li];
                        {
                            const zf_char *q = p;
                            if (n != 0) {
                                do {
                                    if (*e == 0 || *q != *e) break;
                                    q++;
                                    e++;
                                    u10++;
                                } while (u10 < n);
                            }
                            if (*e != 0) {
                                if (*q <= *e) goto b6f;
                                continue;
                            }
                        }
                        u7 = u10;
                        if (u10 == n) goto b6f;
                    }
                b6f:
                    if (l2 && u7 + 3 <= n && p[u7] == 0x28) {
                        uint32_t u4b = u7 + 1, r = prefix_find(p + u4b, (n - u4b) - 1, 1, l2, NULL);
                        if (r != 0) {
                            u4b += r;
                            if (p[u4b] == 0x29) u7 = u4b + 1;
                        }
                    }
                    return u7;
                }
            b44:
                i12 = i3 + 1;
            }
        }
        i12 = i13;
    } while (i13 <= i14);
    return 0;
}

/* ------------------------------------------------------------------ state emulation */
#define NW 0xe0
typedef struct sstate {
    uint32_t w[NW];                     /* byte offset o -> w[o >> 2] */
    const zf_char *text;                /* +0x2e0 (and +0x08 scanner text: always the same buffer) */
    const zf1_ss *m;                    /* model (+0x88 / +0x2c8) and char table (+0x318) */
} sstate;

typedef struct ssave { uint32_t w[0x8a]; } ssave;   /* FUN_180162738 save area (int[]) */

#define W(o) (st->w[(o) >> 2])
#define SI(o) (*(int32_t *)&st->w[(o) >> 2])
#define TXT (st->text)
#define LANG 9u                          /* model vt+0x68 = 0x409 & 0x3ff */

/* token field helpers (t = token byte offset) */
#define TTYPE(t) SI((t) + 8)
#define TSTART(t) W((t) + 0xc)
#define TLEN(t) W((t) + 0x10)
#define TFLAGS(t) W((t) + 0x14)
#define TEND(t) SI((t) + 0x18)
#define TCH0(t) TXT[TSTART(t)]

static void tok_copy(sstate *st, int dst, int src)   /* 18007b2e4 (8 words incl. base pointer) */
{
    int k;
    if (dst == src) return;
    for (k = 0; k < 8; k++) st->w[(dst >> 2) + k] = st->w[(src >> 2) + k];
}

/* 1800774b4 */
static void tok_set(sstate *st, int t, int type, uint32_t start, uint32_t len, uint32_t flags)
{
    W(t) = 1;                            /* base pointer = text (non-null marker) */
    W(t + 4) = 0;
    SI(t + 8) = type;
    if (type == 0) { W(t + 0xc) = 0; W(t + 0x10) = 0; }
    SI(t + 0x18) = 0;
    W(t + 0x10) = len;
    W(t + 0x14) = flags;
    W(t + 0xc) = start;
}

static int tok_isnul(sstate *st, int t) { return TLEN(t) == 1 && TCH0(t) == 0; }     /* 18007b288 */
static int tok_atend(sstate *st, int t) { return TEND(t) != 0 || tok_isnul(st, t); }  /* 18007ad50 */
/* 180161db0 */
static int tok_isnum(sstate *st, int t) { return (TTYPE(t) == 3 && iswdigit_(TCH0(t))) || TTYPE(t) == 4; }
/* 180161d5c: token text equals s (case-insensitive ASCII, _wcsnicmp in the C locale) */
static int tok_eq(sstate *st, int t, const char *s)
{
    uint32_t n = (uint32_t)strlen(s), k;
    if (n != TLEN(t)) return 0;
    for (k = 0; k < n; k++)
        if (c_tolower_ascii((zf_char)(unsigned char)s[k]) != c_tolower_ascii(TXT[TSTART(t) + k])) return 0;
    return 1;
}
/* 1801628f4: initials "A.B." */
static int tok_initials(sstate *st, int t)
{
    uint32_t n = TLEN(t), k;
    const zf_char *p = TXT + TSTART(t);
    int dot = 0;
    for (k = 0; k < n; k++, p++) {
        if (dot) {
            if (*p != 0x2e) return 0;
            dot = 0;
        } else {
            if (c_upper(*p) == 0) return 0;
            dot = 1;
        }
    }
    return 1;
}
/* 18016296c: roman numeral letters only */
static int tok_roman(sstate *st, int t)
{
    uint32_t k;
    for (k = 0; k < TLEN(t); k++)
        if (!strchr("iIvVxXlLcCdDmM", (int)(TXT[TSTART(t) + k] < 128 ? TXT[TSTART(t) + k] : 1)) || TXT[TSTART(t) + k] == 0) return 0;
    return 1;
}
/* 180162aa4 */
static int tok_2aa4(sstate *st, int t)
{
    uint32_t n = TLEN(t), k;
    int s = 1;
    const zf_char *p;
    if (TTYPE(t) != 3 || n < 2) return 0;
    p = TXT + TSTART(t);
    for (k = 0; k < n; k++, p++) {
        if (s == 1) {
            if (iswdigit_(*p)) return 0;
            s = 2;
        } else if (s == 2) {
            if (*p == 0x2e) return 0;
            s = 3;
        } else if (s == 3 && *p == 0x2e) return 0;
    }
    if (s == 3) return tok_roman(st, t) == 0;
    return 0;
}

/* lowercase copy (FUN_180058730 + FUN_1801631c8) and lookup */
static int lc_find(const zf1_ss *m, int li, const zf_char *s, uint32_t n)
{
    zf_char b[128];
    uint32_t k;
    if (n >= 0x80) return 0;
    for (k = 0; k < n; k++) {
        if (s[k] == 0) break;
        b[k] = s[k];
    }
    for (; k < n; k++) b[k] = 0;     /* copy stops at a 0 */
    for (k = 0; k < n; k++) b[k] = c_tolower(b[k]);
    return list_find(&m->list[li], b, n) >= 0;
}
/* 180161740 numeric prefixes (fig. no. ...) */
static int is_numpfx(const zf1_ss *m, const zf_char *s, uint32_t n) { return lc_find(m, L_NUMPFX, s, n); }
/* 1801617e4 digit words */
static int is_digitw(const zf1_ss *m, const zf_char *s, uint32_t n) { return lc_find(m, L_DIGITW, s, n); }
/* 180161624 capitalized sentence starters */
static int is_capstart(const zf1_ss *m, const zf_char *s, uint32_t n)
{
    uint32_t k = 0;
    if (!c_upper(s[0])) return 0;
    while (k < n && !sq_(s[k])) k++;
    return list_find(&m->list[L_CAPSTART], s, k) >= 0;
}
/* 180161888 titles */
static int is_title(const zf1_ss *m, const zf_char *s, uint32_t n, int capfirst)
{
    zf_char b[128];
    uint32_t k;
    if (n >= 0x80) return 0;
    if (!capfirst) return list_find(&m->list[L_TITLE], s, n) >= 0;
    for (k = 0; k < n && s[k]; k++) b[k] = s[k];
    for (; k < n; k++) b[k] = 0;
    for (k = 0; k < n; k++) b[k] = c_tolower(b[k]);
    b[0] = c_toupper(b[0]);
    return list_find(&m->list[L_TITLE], b, n) >= 0;
}

/* FUN_18005bbcc on the CCharTable: flags of code point c (bit 0 tested by the caller) */
static int ctab_flags(const zf1_ss *m, uint32_t c, uint32_t *flags)
{
    int lo = 0, hi = m->ctab.n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = zf_rd32(m->ctab.rec + 16 * mid);
        if (v == c) { *flags = zf_rd32(m->ctab.rec + 16 * mid + 4); return 1; }
        if (v < c) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* 180161120: abbreviation test on s[0..n) (list0 + shape rules); *allcap set for "A.B." shapes */
static int is_abbr(const zf1_ss *m, const zf_char *s, uint32_t n, int use_ctab, char *allcap)
{
    const zf_char *e = s + n, *p = s, *p2;
    zf_char b[128];
    uint32_t u4;
    int r;
    while (p < e && c_alpha(*p) == 0 && c_alnum(*p) != 0) p++;
    if (p + 1 < e && ((*p - 0x2c) & 0xfffd) == 0) {
        p2 = p + 1;
        for (;;) {
            p = p2;
            if (c_alpha(*p) != 0) break;
            if (c_alnum(*p) == 0) break;
            p = p + 1;
            p2 = p;
            if (e <= p) break;
        }
    }
    u4 = n - (uint32_t)(p - s);
    if (p == s || 2 < u4) {
        if (u4 < 0x80) {
            uint32_t k;
            int hasv, anyl;
            if (list_find(&m->list[L_ABBR], p, u4) >= 0) return 1;
            for (k = 0; k < u4 && p[k]; k++) b[k] = p[k];
            for (; k < u4; k++) b[k] = 0;
            b[u4 < 128 ? u4 : 127] = 0;
            if (c_upper(*p) != 0 && 2 < u4) {
                uint32_t kk;
                for (kk = 0; kk < u4; kk++) b[kk] = c_tolower(b[kk]);
                if (list_find(&m->list[L_ABBR], b, u4) >= 0) return 1;
                for (k = 0; k < u4 && p[k]; k++) b[k] = p[k];
                for (; k < u4; k++) b[k] = 0;
                b[u4] = 0;
                /* vowel / lower-case scan */
                hasv = 0;
                anyl = 0;
                {
                    const zf_char *q = b;
                    zf_char c0 = b[0];
                    while (c0 != 0) {
                        zf_char c3 = c0;
                        if (!hasv) {
                            c3 = *q;
                            if (c3 < 0x100) {
                                if (!(ctb(c3) & 0x80)) {
                                    if (zf1_base3c50[c3 & 0xff] == 0x79) hasv = 1;
                                } else hasv = 1;
                            } else if (use_ctab) {
                                uint32_t fl;
                                if (ctab_flags(m, c3, &fl) && (fl & 1)) hasv = 1;
                            }
                        }
                        anyl |= c_lower(c3) != 0;
                        q++;
                        c0 = *q;
                    }
                }
                if (!hasv && anyl) return 1;
            } else {
                /* (the copy above) */
            }
            if (3 < u4 && (u4 & 1) == 0) {
                int allu = allcap != NULL;
                uint32_t i = 0;
                const zf_char *q = b;
                do {
                    if ((i & 1) == 0) {
                        if (c_alpha(*q) == 0) break;
                        allu = allu && c_upper(*q) != 0;
                    } else {
                        if (!dot_(*q)) break;
                    }
                    i++;
                    q++;
                } while (i < u4);
                if (u4 <= i) {
                    if (allcap && allu) *allcap = 1;
                    return 1;
                }
            }
            /* LANG == 10 (Spanish) rule not used */
            if (p != s) goto l519;
        }
        return 0;
    }
l519:
    r = list_find(&m->list[L_ABBR], s, n) >= 0;
    return r;
}

/* 18003fbe4: emoticon-like shapes starting with ':' ';' '8' '>' '}' '=' '%' ... */
static int emo_shape(const zf_char *T, uint32_t pos, uint32_t *plen)
{
    uint32_t n = *plen;
    const zf_char *p5 = T + pos, *p6 = p5 + 1;
    zf_char c = *p5;
    int b;
    int32_t i7;
    uint32_t u2;
    if (n < 3 || c != 0x3a) {
        if (c == 0x3e || c == 0x7d) {
            n = n - 1;
            if (n == 0) return 0;
            c = *p6;
            p6 = p5 + 2;
        }
        if (2 < n && c == 0x38) {
            if (*p6 != 0x2f) goto ccd;
            u2 = lowdigit_(p6[1]);
            b = u2 == 0;
            goto c71b;
        }
        if (c == 0x3a || c == 0x3b || c == 0x38) goto ccd;
        b = c == 0x25;
    } else {
        if (*p6 == 0x2f && p5[2] == 0x2f) return 0;
        if (lowdigit_(*p6) && lowdigit_(p5[2])) return 0;
        if (*p6 != 0x5c) goto ccd;
        u2 = c_alnum(p5[2]);
        b = u2 == 0;
    }
c71b:
    if (!b) return 0;
ccd:
    i7 = (int32_t)n - 1;
    if (i7 != 0) {
        zf_char c4 = *p6;
        const zf_char *q = p6 + 1;
        if (c4 == 0x2d || c4 == 0x5e) {
            i7 = (int32_t)n - 2;
            if (i7 == 0) return 0;
            c4 = *q;
            q = p6 + 2;
        }
        if (c4 == 0x6f || ((zf_char)(c4 - 0x28) < 0x35 && ((0x10008210500081ULL >> ((c4 - 0x28) & 0x3f)) & 1)) ||
            c4 == 0x7c || ((c4 == 0x30 || c4 == 0x29) && q[-2] != 0x38)) {
            uint32_t u3 = (uint32_t)(q - (T + pos));
            const zf_char *q6 = q;
            *plen = u3;
            if ((zf_char)(c4 - 0x28) < 2 && (i7 = i7 - 1) != 0) {
                c4 = *q;
                q6 = q + 1;
                if (c4 == q[-1]) *plen = u3 + 1;
            }
            if (c_alpha(c4) != 0 && i7 != 1) return c_alnum(*q6) == 0;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ tokenizer (18003dbc0) */
#define SC_TOKSTART 0x14
#define SC_POS 0x18
#define SC_LEN 0x10

static void lex(sstate *st, int tok, uint32_t flag3)
{
    const zf_char *T = TXT;
    const zf1_ss *m = st->m;
    uint32_t u21 = W(SC_TOKSTART), u28, u16, u9, u22;
    int32_t i8, i23, i30;
    uint32_t u29 = 0;
    zf_char c26;
    W(SC_POS) = u21;
    if (u21 == W(SC_LEN)) {
        i23 = 0x11;
        i8 = 0;
        u29 = 0;
        goto l2b2;
    }
    c26 = T[u21];
    if (c26 == 0) {
        i23 = 0xf;
        i8 = 1;
        u29 = 0;
        goto l2b2;
    }
    if (499 < u21) {
        tok_set(st, tok, 0xf, u21, 0, 0);
        TEND(tok) = 1;
        goto l2ba;
    }
    u28 = W(SC_LEN) - u21;
    if (u28 >= 2 && hi_(T[u21]) && lo_(T[u21 + 1])) {   /* 18003fdb8 */
        tok_set(st, tok, 3, u21, 2, 0);
        W(SC_POS) += 2;
        goto l2ba;
    }
    if (nl_(c26)) {
        uint32_t nx;
        int b6, b5;
        i8 = 1;
        tok_set(st, tok, 0x10, u21, 1, 0);
        nx = W(SC_POS) + 1;
        W(SC_POS) = nx;
        if (W(SC_LEN) <= nx) goto l2ba;
        b6 = (c26 == 10 || c26 == 0x8d);
        b5 = (T[nx] == 10 || T[nx] == 0x8d);
        if ((c26 != 0xd || !b5) && (!b6 || T[nx] != 0xd)) goto l2ba;
        TLEN(tok) = 2;
        W(SC_POS) += (uint32_t)i8;
        goto l2ba;
    }
    {
        uint32_t lr8 = u28;
        if (emo_shape(T, u21, &lr8)) {
            u29 = 0;
            i8 = 0x12;
            u28 = lr8;
            goto ld81;
        }
    }
    u16 = u21;
    u9 = c_alnum(c26);
    i8 = 1;
    u22 = flag3;
    if (u9 == 0) {
        u9 = c_space(c26);
        if (u9 == 0 && !quote_(c26) && 1 < u28 &&
            ((flag3 & 8) == 0 || (T[u21 - 1] != 0x5f && c_alnum(T[u21 - 1]) == 0))) {
            uint32_t r;
            u21 = W(SC_POS);
            u16 = u21;
            r = prefix_find(T + u21, W(SC_LEN) - u21, 1, &m->list[L_EMOTI], &m->list[L_SUFFIX]);
            u28 = r;
            if (u28 != 0) {
                u29 = 0x41;
                i8 = 3;
                goto ld81;
            }
        }
    }
    u21 = u16;
    if (c26 == 0x2026) {
        i23 = 8;
        goto leac;
    }
    if (excl_(c26)) { i23 = 0xc; goto leac; }
    if (cjkstop_(c26)) { i23 = 0x13; goto leac; }
    if (!ws_(c26)) {
        if ((zf_char)(c26 + 0x1800) < 2) {
            u28 = u21 + 1;
            u16 = u28;
            W(SC_POS) = u28;
            while (u28 < W(SC_LEN)) {
                zf_char s3 = T[u16];
                if (s3 != 0xe800 && s3 != 0xe801) break;
                u28 = u16 + 1;
                u16 = u28;
                W(SC_POS) = u28;
            }
            i8 = (int32_t)(u16 - u21);
            i23 = 2;
            goto l2ac;
        }
        if (SI(0x90) != 0) {
            if (!open_(c26)) {
                if (!close_(c26)) goto lfa9;
                i23 = 0xb;
                i30 = quote_(c26) ? 0x20 : 0x10;
            } else {
                i23 = 10;
                i30 = quote_(c26) ? 0x20 : 0x10;
            }
            goto leb0;
        }
    lfa9:
        if (apos_(c26)) {
            i30 = 4;
            if (flag3 == 0) {
                uint32_t r, p0 = W(SC_POS);
                r = prefix_find(T + p0, W(SC_LEN) - p0, 1, &m->list[L_EMOTI], &m->list[L_SUFFIX]);
                if (r != 0) {
                    i30 = 0x45;
                    i8 = (int32_t)r;
                }
            }
            i23 = quote_(c26) ? 0xd : 0x1f;
            goto leb0;
        }
        if (quote_(c26) || ((c26 - 0xab) & 0xffef) == 0) { i23 = 0xd; goto leac; }
        if (dot_(c26)) {
            uint32_t u13 = 3, u25 = 4, u12 = 0, u10;
            int32_t i8b;
            u22 = u21 + 1;
            W(SC_POS) = u22;
            u16 = 1;
            u28 = u22;
            i23 = 3;
            do {
                for (;;) {
                    if (u28 < W(SC_LEN)) c26 = T[u28];
                    else c26 = (zf_char)u12;
                    i8b = (int32_t)u16;
                    if (i8b != 1) break;
                    if (c_space(c26) == 0) {
                        if (!dot_(c26)) {
                            i23 = (int32_t)u13;
                            goto l18c;
                        }
                        u28 = u28 + 1;
                        u16 = 2;
                        W(SC_POS) = u28;
                    } else {
                        u28 = u28 + 1;
                        u16 = u13;
                        W(SC_POS) = u28;
                    }
                }
                u9 = u28;
                if (i8b == 2) {
                    if (u28 - u21 < u13 && dot_(c26)) {
                        u28 = u28 + 1;
                        W(SC_POS) = u28;
                    } else {
                    l17f:
                        u16 = 0xffffffffu;
                        u22 = u9;
                    }
                } else if (i8b == 3) {
                    u9 = u22;
                    if (!dot_(c26)) goto l17f;
                    u28 = u28 + 1;
                    u16 = u25;
                    W(SC_POS) = u28;
                } else if (i8b == 4) {
                    u10 = (u28 - u21) >> 1;
                    if (u10 != 2 && u10 != u25) {
                        u9 = u22;
                        if (u10 == 6) {
                            u16 = 0xffffffffu;
                            u9 = u28;
                        }
                    }
                    u22 = u9;
                    if (0 < (int32_t)u16) {
                        if (c_space(c26) == 0) goto l17f;
                        u28 = u28 + 1;
                        u16 = u13;
                        W(SC_POS) = u28;
                    }
                }
                i23 = (int32_t)u13;
            } while (0 < (int32_t)u16);
        l18c:
            W(SC_POS) = u22;
            i8 = (int32_t)(u22 - u21);
            u29 = u12;
            if ((uint32_t)(i8 - 1) < 2) i23 = 0xc;
            else i23 = (i8 != i23) + 8;
            goto l2b2;
        }
        if (comma_(c26)) { i23 = 0xe; goto leac; }
        if (sym1_(c26)) {
            i23 = 0x1b;
            i30 = (c26 == 0xa7) ? 0x80 : 0;
            goto leb0;
        }
        if (math_(c26)) { i23 = 0x1d; goto leac; }
        if (bullet_(c26)) { i23 = 0x1c; goto leac; }
        if (curr_(c26)) { i23 = 0x1e; goto leac; }
        if (dash_(c26)) { i23 = 0x20; goto leac; }
        if (invq_(c26)) { i23 = 0x1a; goto leac; }
        u28 = c_alnum(c26);
        if (u28 != 0) {
            /* ---------------- word / number / URL scanner ---------------- */
            uint32_t local_50 = 0, local_64 = 0, local_4c = 0, local_6c, local_74, u13, u27, u10, u11, u4;
            uint64_t local_58;
            char cVar19, local_68 = 0, local_67 = 0, lr8b = 0;
            char local_70 = 0;
            u28 = 0;
            if ((u22 & 2) == 0) {
                cVar19 = 0;   /* model vt+0x60 returns 0 -> cVar19 = 0 */
            } else {
                cVar19 = 0;
            }
            local_58 = u21;
            u13 = u16;
            u22 = 0;
            local_74 = u21;
            local_6c = u21;
            for (;;) {
                u9 = (uint32_t)u13;
                u27 = 0xffffffffu;
                u11 = 0xffffffffu;
                u10 = u22;
                if ((int32_t)u22 < 0) break;
                u4 = W(SC_POS);
                if (u4 < W(SC_LEN)) c26 = T[u4];
                else c26 = 0;
                u10 = u11;
                if ((1 < (int32_t)(W(SC_LEN) - u4) && hi_(T[u4]) && lo_(T[u4 + 1])) || cjkstop_(c26)) break;
                u11 = c_alnum(c26);
                if (u11 == 0 && c26 != 0x5f &&
                    (sym1_(c26) || math_(c26) || bullet_(c26) || curr_(c26) || invq_(c26) || comma_(c26) ||
                     dash_(c26) || c26 == 0x2d || apos_(c26) || dot_(c26) || c26 == 0x21)) {
                    if (local_68 == 0 && W(SC_POS) < W(SC_LEN) && (apos_(c26) || !quote_(c26))) {
                        uint32_t r;
                        local_68 = 1;
                        r = prefix_find(T + local_74, W(SC_LEN) - local_74, (W(SC_POS) - local_74) + 1,
                                        &m->list[L_EMOTI], &m->list[L_SUFFIX]);
                        u9 = local_6c;
                        if (r != 0) {
                            u28 |= 0x41;
                            W(SC_POS) = r + local_74;
                            break;
                        }
                    }
                    if (c26 != 0x2d && !apos_(c26) && !dot_(c26)) {
                        u10 = 0xffffffffu;
                        if (flag3 != 0) break;
                        if (u9 == local_74) {
                            u9 = W(SC_POS);
                            lr8b = cVar19;
                            local_6c = u9;
                            local_64 = u28;
                        }
                    }
                }
                u13 = local_74;
                u10 = u28;
                if ((int32_t)u22 < 0x3d) {
                    if (u22 == 0x3c) {
                        if (c_digit(c26) == 0) {
                            if (!dot_(c26)) {
                                if (c26 == 0x2c || c26 == 0x2d || c26 == 0x3a) goto edc4;
                                u22 = 0x42;
                            } else {
                                u22 = u27;
                                if (flag3 == 0) {
                                edc4:
                                    if (u9 == local_74) {
                                        local_6c = W(SC_POS);
                                        lr8b = cVar19;
                                        local_64 = u28;
                                    }
                                    W(SC_POS) += 1;
                                    u22 = 0x3d;
                                }
                            }
                        } else {
                            W(SC_POS) += 1;
                        }
                        goto e5d1;
                    }
                    if (u22 == 0) {
                        u22 = c_digit(c26);
                        if (u22 == 0) {
                        ed24:
                            u22 = c_alpha(c26);
                            if (u22 != 0) goto ed31;
                        ecd1:
                            W(SC_POS) += 1;
                            u22 = 10;
                            u10 = u28;
                        } else {
                            W(SC_POS) += 1;
                            u22 = 0x3c;
                            u10 = u28 | 2;
                        }
                        goto e5d1;
                    }
                    if (u22 == 10) {
                        char cVar7 = cVar19;
                        u22 = flag3;
                        if (dot_(c26)) {
                            if (u9 == (uint32_t)u13) {
                                local_6c = W(SC_POS);
                                lr8b = cVar7;
                                local_64 = u28;
                            }
                        e94d:
                            W(SC_POS) += 1;
                            u22 = 0x14;
                            goto e5d1;
                        }
                        if (!apos_(c26)) {
                            u22 = c_digit(c26);
                            if (u22 != 0) {
                                u28 = u28 | 2;
                                goto ecd1;
                            }
                            if (c26 == 0xad || ((u22 = c_alnum(c26)) != 0 && c26 != 0xe800 && c26 != 0xe801) ||
                                c26 == 0x5f || c26 == 0x2010 || c26 == 0x2011)
                                goto ed24;
                        ea0e:
                            u22 = u27;
                            if (c26 == 0x28) u22 = 0x46;
                            goto e5d1;
                        }
                        if (cVar19 == 0) {
                            if (u22 == 0) {
                                if (u9 == (uint32_t)u13) {
                                    local_6c = W(SC_POS);
                                    lr8b = 0;
                                    local_64 = u28;
                                }
                                local_58 = W(SC_POS);
                                W(SC_POS) += 1;
                                u27 = 0x28;
                                u28 = u28 | 4;
                            }
                            goto efe9;
                        }
                        u10 = u28 | 4;
                        local_58 = W(SC_POS);
                        u22 = 0x28;
                        W(SC_POS) += 1;
                        goto e5eb;
                    }
                    if (u22 == 0x14) {
                        u22 = c_alnum(c26);
                        if (u22 == 0 || c26 == 0xe800 || c26 == 0xe801) {
                            if (c26 != 0x2d) {
                                if (apos_(c26)) {
                                    u22 = u27;
                                    if (flag3 == 0) {
                                        if (u9 == local_74) {
                                            local_6c = W(SC_POS);
                                            lr8b = cVar19;
                                            local_64 = u28;
                                        }
                                        u22 = W(SC_POS);
                                        u28 = u28 | 4;
                                        local_58 = u22;
                                    e873:
                                        W(SC_POS) = u22 + 1;
                                        goto e835;
                                    }
                                    goto e5d1;
                                }
                                if (c_space(c26) == 0 || (flag3 & 2) != 0 || (u28 & 4) != 0) {
                                e909:
                                    u22 = 0x32;
                                } else {
                                    uint32_t r = prefix_find(T + local_74, W(SC_LEN) - local_74, (W(SC_POS) - local_74) + 1,
                                                             &m->list[L_ABBR], NULL);
                                    if (r == 0) {
                                        r = prefix_find(T + local_74, W(SC_LEN) - local_74, (W(SC_POS) - local_74) + 1,
                                                        &m->list[L_TITLE], NULL);
                                        if (r == 0) goto e909;
                                    }
                                    W(SC_POS) = r + local_74;
                                    u22 = 0xfffffffeu;
                                }
                                goto e5d1;
                            }
                            if ((flag3 & 4) == 0 &&
                                is_abbr(m, T + local_74, W(SC_POS) - local_74, 1, &local_70)) {
                                local_64 = u28;
                                if (local_70 != 0) local_64 = u28 | 8;
                                local_6c = W(SC_POS);
                                local_4c = 5;
                                u28 = local_64;
                            }
                        } else {
                        ea28:
                            if (SI(0x7c) == 0 && c_upper(c26) != 0) SI(0x7c) = 1;
                            else if (SI(0x80) == 0 && c_lower(c26) != 0) SI(0x80) = 1;
                        }
                    e996:
                        W(SC_POS) += 1;
                        u22 = 0x1e;
                        u10 = u28;
                        goto e5d1;
                    }
                    if (u22 == 0x1e) {
                        if (dot_(c26)) goto e94d;
                        if (!apos_(c26)) {
                            u22 = c_digit(c26);
                            if (u22 == 0) {
                                if (c26 == 0xad || ((u22 = c_alnum(c26)) != 0 && c26 != 0xe800 && c26 != 0xe801) ||
                                    c26 == 0x5f)
                                    goto ea28;
                                if (c26 != 0x2d) goto ea0e;
                                u22 = u27;
                                if (flag3 != 0) goto e5d1;
                                if (u9 == local_74) {
                                    local_6c = W(SC_POS);
                                    lr8b = cVar19;
                                    local_64 = u28;
                                }
                            } else {
                                u28 = u28 | 2;
                            }
                            goto e996;
                        }
                        if ((u28 & 4) != 0) {
                            uint32_t p = W(SC_POS);
                            W(SC_POS) = p + 1;
                            u22 = 0x28;
                            local_58 = p;
                            goto e5d1;
                        }
                    ef13:
                        u22 = 0xffffffffu;
                        goto e5d1;
                    }
                    if (u22 != 0x28) {
                        if (u22 == 0x32) {
                            if ((flag3 & 4) == 0) {
                                uint32_t n0 = W(SC_POS) - local_74;
                                u13 = local_74;
                                u22 = is_title(m, T + local_74, n0, 0);
                                if (u22 == 0) {
                                    if (!is_abbr(m, T + local_74, n0, 1, &local_70)) {
                                        u28 = is_title(m, T + local_74, n0, 1);
                                        if (u28 != 0) goto e5c9;
                                        if (list_find(&m->list[L_BIBLE], T + local_74, n0) < 0) {
                                            u9 = local_6c;
                                            goto e6e1;
                                        }
                                        u22 = 0xfffffffbu;
                                    } else {
                                        u22 = 0xfffffffeu;
                                        if (local_70 != 0) u10 = u28 | 8;
                                    }
                                } else {
                                e5c9:
                                    u22 = 0xfffffffdu;
                                }
                            } else {
                            e6e1:
                                if (flag3 == 0 && local_74 < u9) {
                                    uint32_t k = u9;
                                    if (!dash_(T[u9])) {
                                        uint32_t k2 = W(SC_POS) - 1;
                                        k = k2;
                                        if (u9 < k2) {
                                            do {
                                                if (dash_(T[k])) break;
                                                k = k - 1;
                                            } while (u9 < k);
                                        }
                                    }
                                    if (dash_(T[k]) &&
                                        is_abbr(m, T + k + 1, (W(SC_POS) - k) - 1, 1, NULL)) {
                                        u22 = 0xffffffffu;
                                        goto e5d1;
                                    }
                                }
                                W(SC_POS) -= 1;
                                u22 = 0xffffffffu;
                            }
                        }
                        goto e5d1;
                    }
                    /* u22 == 0x28 */
                    if (c_alnum(c26) != 0 ? (zf_char)(c26 + 0x1800) >= 2 : (c26 == 0xad)) goto l28a;
                    if (c26 == 0x5f) goto l28a;
                    if (c26 == 0x2d) goto e805;
                    u22 = W(SC_POS);
                    u13 = c26;
                    if (!apos_(c26)) {
                        if (dot_((zf_char)u13)) {
                            if (u22 < W(SC_LEN)) {
                                zf_char pc = T[u22 - 1], nc = T[u22 + 1];
                                int a1 = c_alnum(pc) != 0 ? (zf_char)(pc + 0x1800) >= 2 : (pc == 0xad);
                                int a2 = c_alnum(nc) != 0 ? (zf_char)(nc + 0x1800) >= 2 : (nc == 0xad);
                                if (pc == 0x5f) a1 = 1;
                                if (nc == 0x5f) a2 = 1;
                                if (a1 && a2) {
                                    local_74 = (uint32_t)local_58 + 1;
                                    cVar19 = 0;
                                    u27 = 10;
                                    W(SC_POS) = local_74;
                                    goto efe9;
                                }
                            }
                            u22 = ((cVar19 != 0) ? 0x51u : 0u) - 1;
                            goto e5d1;
                        }
                        u9 = u22 - 1;
                        if (rsq_(T[u9]) && dot_(T[u22 - 2])) {
                            W(SC_POS) = u9;
                            goto e909;
                        }
                        if (c26 == 0x28) {
                            u22 = 0x46;
                            goto e5d1;
                        }
                        u27 = ((cVar19 != 0) ? 0x51u : 0u) - 1;
                    } else {
                        u9 = c_alpha(T[u22 - 1]);
                        if (u9 != 0) goto e873;
                        cVar19 = 0;
                    }
                    goto efe9;
                l28a:
                    u22 = c_alpha(c26);
                    if (u22 != 0) {
                        u28 = u28 | 1;
                        if (SI(0x7c) == 0 && c_upper(c26) != 0) SI(0x7c) = 1;
                        else if (SI(0x80) == 0 && c_lower(c26) != 0) SI(0x80) = 1;
                    }
                    u9 = c_digit(c26);
                    W(SC_POS) += 1;
                    u22 = 0x28;
                    u10 = u28 | 2;
                    if (u9 == 0) u10 = u28;
                    goto e5d1;
                } else {
                    if (u22 == 0x3d) {
                        u28 = c_digit(c26);
                        if (u28 == 0) {
                            int32_t p = SI(SC_POS);
                            if (!dot_(c26) || !dot_(T[p - 1])) {
                                SI(SC_POS) = p - 1;
                                u22 = 0x42;
                            } else {
                                SI(SC_POS) = p + 1;
                            }
                        } else {
                            W(SC_POS) += 1;
                            u22 = 0x3c;
                        }
                    } else if (u22 == 0x42) {
                        u22 = c_alpha(c26);
                        if (u22 != 0 || c26 == 0x5f) {
                        ed31:
                            u28 = u28 | 1;
                            if (SI(0x7c) == 0 && c_upper(c26) != 0) SI(0x7c) = 1;
                            else if (SI(0x80) == 0 && c_lower(c26) != 0) SI(0x80) = 1;
                            goto ecd1;
                        }
                        if (!apos_(c26)) {
                            if (!dot_(c26)) goto ea0e;
                            W(SC_POS) += 1;
                            u22 = 0xfffffffau;
                        } else {
                        e805:
                            u22 = u27;
                            if (flag3 == 0) {
                                if (u9 == local_74) {
                                    local_6c = W(SC_POS);
                                    lr8b = cVar19;
                                    local_64 = u28;
                                }
                                W(SC_POS) += 1;
                            e835:
                                u22 = 0x28;
                                u10 = u28;
                            }
                        }
                    } else if (u22 == 0x46) {
                        W(SC_POS) += 1;
                        local_50 = W(SC_POS);
                        u22 = 0x47;
                    } else if (u22 == 0x47) {
                        u9 = c_alpha(c26);
                        u28 = local_50;
                        if (u9 == 0) {
                            if (c26 == 0x29) {
                                uint32_t lo50 = local_50;
                                u22 = W(SC_POS);
                                if (local_50 < u22) {
                                    if (list_find(&m->list[L_SUFFIX], T + lo50, u22 - u28) >= 0) {
                                        W(SC_POS) += 1;
                                        u22 = 0xffffffffu;
                                        goto e5d1;
                                    }
                                }
                            }
                            W(SC_POS) = u28 - 1;
                            goto ef13;
                        }
                        if (SI(0x7c) == 0 && c_upper(c26) != 0) SI(0x7c) = 1;
                        else if (SI(0x80) == 0 && c_lower(c26) != 0) SI(0x80) = 1;
                        W(SC_POS) += 1;
                    } else if (u22 == 0x50 && (u22 = u27, cVar19 != 0)) {
                        uint32_t p9 = W(SC_POS) - 1;
                        if (apos_(T[p9])) {
                            W(SC_POS) = p9;
                            if ((uint32_t)local_58 == p9) {
                                local_58 = u13;
                                u10 = u28 & 0xfffffffbu;
                            }
                        }
                    }
                e5d1:
                    u27 = u22;
                    u28 = u10;
                    if (cVar19 != 0 && ((int32_t)u22 < 0x50 || 0x59 < (int32_t)u22)) {
                    e5eb:
                        u27 = u22;
                        u28 = u10;
                        if (!apos_(c26) && !dot_(c26)) {
                            if (c_alpha(c26) == 0 && c26 != 0x5f) cVar19 = 0;
                            else cVar19 = 1;
                        }
                    }
                }
            efe9:
                u13 = local_6c;
                u22 = u27;
                if (499 < W(SC_POS)) {
                    local_67 = 1;
                    u22 = 0xffffffffu;
                }
            }
            /* after the loop */
            {
                uint32_t u6 = W(SC_POS), u9b, u27b;
                u22 = local_6c;
                if (u21 < local_74) u10 = 0xffffffffu;
                u9b = u6 - 1;
                u27b = u6;
                while (T[u9b] == 0x5f) {
                    W(SC_POS) = u27b - 1;
                    u9b = u27b - 2;
                    u27b = u27b - 1;
                }
                if (u10 == 0xfffffffeu) u9b = 5;
                else if (u10 == 0xfffffffau) { i30 = 1; u9b = 4; goto setlocal60; }
                else if (u10 == 0xfffffffbu) u9b = 6;
                else if (u10 == 0xfffffffdu) u9b = 7;
                else {
                    u9b = 0;   /* local_5c (0 here: only set to 3 in the period scanner, which returned) */
                    if (u10 == 0xffffffffu) {
                        uint32_t local_5c = 3, local_60 = 0;
                        if (u21 < local_6c && local_6c < u27b) {
                            uint32_t r = 0;
                            if (flag3 == 0) {
                                if ((u28 & 0x40) == 0) {
                                    u27b = W(SC_POS);
                                    r = prefix_find(T + u21, u27b - u21, u27b - u21, &m->list[L_EMOTI],
                                                    &m->list[L_SUFFIX]);
                                    if (r != 0) u28 |= 0x40;
                                }
                                local_60 = (r == 0);
                            } else {
                                W(SC_POS) = local_6c;
                                u28 = local_64;
                                u27b = local_6c;
                                if (local_4c != 0) local_5c = local_4c;
                            }
                        }
                        u9b = local_5c;
                        if ((u28 & 4) != 0) {
                            uint32_t u10b = (uint32_t)local_58;
                            if (local_60 != 0 && u10b < u22) cVar19 = lr8b;
                            if (cVar19 == 0) {
                                if (flag3 != 0) {
                                    if (u10b < u27b) {
                                        W(SC_POS) = u10b;
                                        u27b = u10b;
                                    }
                                    u28 = 1;
                                }
                            }
                        }
                    }
                }
                goto wordset;
            setlocal60:
                ;
            wordset:
                tok_set(st, tok, (int32_t)u9b, u21, u27b - u21, u28);
                if (local_67 != 0) TEND(tok) = 1;
                (void)i30;
                goto l2ba;
            }
        }
        {
            int b6 = excl_(c26) || cjkstop_(c26) || c26 == 0x2024 || c26 == 0x2025 || c26 == 0x2026 || c26 == 0x203d ||
                     c26 == 0xff0e;
            i30 = 0;
            i8 = 1;
            i23 = b6 ? 0xc : 0xf;
            goto l25f;
        }
    }
    /* whitespace run */
    u28 = u21 + 1;
    u16 = u28;
    W(SC_POS) = u28;
    while (u28 < W(SC_LEN)) {
        zf_char c = T[u16];
        if (nl_(c) || !ws_(c)) break;
        u28 = u16 + 1;
        u16 = u28;
        W(SC_POS) = u28;
    }
    i8 = (int32_t)(u16 - u21);
    i23 = 1;
l2ac:
    u29 = 0;
l2b2:
    tok_set(st, tok, i23, u21, (uint32_t)i8, u29);
    goto l2ba;
leac:
    i30 = 0;
leb0:
l25f:
    tok_set(st, tok, i23, u21, (uint32_t)i8, (uint32_t)i30);
    W(SC_POS) += (uint32_t)i8;
    goto l2ba;
ld81:
    tok_set(st, tok, i8, u21, u28, u29);
    W(SC_POS) += u28;
l2ba:
    tok_copy(st, 0x20, tok);
}

/* ------------------------------------------------------------------ 18003da24: next token */
static void ss_next(sstate *st)
{
    int32_t t, n;
    tok_copy(st, 0xd8, 0xb8);
    tok_copy(st, 0xb8, 0x98);
    W(0xfc) = W(0xc0);
    W(0x14) = W(0x18);
    W(0x20) = W(0x24) = 0;
    W(0x28) = W(0x2c) = W(0x30) = W(0x34) = W(0x38) = 0;
    lex(st, 0x98, (LANG & 0x3ff) == 0x12);
    W(0x2d4) |= W(0x7c);
    W(0x2d8) |= W(0x80);
    if ((SI(0xb0) != 0 && W(0xa8) == 0) || tok_isnul(st, 0x98)) {
        W(0xf8) = W(0xa0);
        return;
    }
    t = SI(0xa0);
    n = SI(0xa8);
    SI(0xf8) = t;
    if (t == 3 || t == 4 || t == 5 || t == 6 || t == 7) {
        SI(0x300) += n;
        SI(0x2fc) += 1;
        if (SI(0x30c) != 0) {
            SI(0x308) += 1;
            goto db84;
        }
    } else {
        if (t != 0xc) {
            if (t == 0xe) {
                if (TXT[W(0xa4)] == 0x3b) SI(0x30c) = 1;
                goto db98;
            }
            if (t == 0x10) {
                SI(0x314) = 1;
                return;
            }
            if (t != 0x12) goto db8b;
        }
    db84:
        SI(0x30c) = 0;
    }
db8b:
    if ((uint32_t)(t - 1) < 2) return;
    if (t == 0x10) return;
db98:
    SI(0x304) += n;
}

/* ------------------------------------------------------------------ bracket stack (state+0x100) and quotes */
#define BK(k) SI(0x100 + 4 * (k))
static int brk_top(sstate *st, int32_t *out)   /* 18016330c */
{
    if (BK(0) == 0 && BK(1) >= 0) {
        *out = BK(BK(1) + 0x34);
        return 1;
    }
    return 0;
}
static void brk_push(sstate *st, int tok)   /* 1801628d0 -> 180163334 (the "expected closer" output is unused) */
{
    int32_t lt = 0, dir = 0, d;
    zf_char c;
    if (BK(0) != 0) return;
    c = TCH0(tok);
    switch (c) {   /* 18016345c */
    case 0x28: case 0xff08: lt = 0; dir = 1; break;
    case 0x29: case 0xff09: lt = 0; dir = -1; break;
    case 0x5b: case 0xff3b: lt = 2; dir = 1; break;
    case 0x5d: case 0xff3d: lt = 2; dir = -1; break;
    case 0x7b: case 0xff5b: lt = 1; dir = 1; break;
    case 0x7d: case 0xff5d: lt = 1; dir = -1; break;
    case 0x2018: case 0x300c: case 0xff62: lt = 3; dir = 1; break;
    case 0x2019: lt = 3; dir = -1; break;
    case 0x201c: lt = 4; dir = 1; break;
    case 0x201d: lt = 4; dir = -1; break;
    case 0x300d: case 0xff63: lt = 5; dir = -1; break;
    case 0x300e: lt = 6; dir = 1; break;
    case 0x300f: lt = 6; dir = -1; break;
    default: break;
    }
    (void)lt;
    if (dir == -1) {
        d = BK(1);
        if (d < 0x32) {
            if (d < 0) return;
            BK(1) = d - 1;
            return;
        }
    } else {
        if (dir != 1) return;
        d = BK(1);
        BK(1) = d + 1;
        if (d + 1 < 0x32) {
            BK(d + 3) = lt;
            BK(BK(1) + 0x34) = (int32_t)TSTART(tok);
            return;
        }
    }
    BK(0) = 1;
}

#define QC(k) SI(0x298 + 4 * (k))
static void quote_count(sstate *st, int tok, int p3, int p4, int p5)   /* 180161040 */
{
    uint32_t pos = TSTART(tok);
    zf_char c = TXT[pos];
    int32_t n;
    if (((c - 0xab) & 0xffef) == 0) {
        n = QC(4);
        if (c == 0xab) {
            if (n == 0) QC(7) = (int32_t)pos;
            n = n + 1;
        } else n = n - 1;
        QC(4) = n;
        return;
    }
    if (TTYPE(tok) != 0xd) c = TXT[TLEN(tok) - 1 + pos];
    if (!dq_(c)) {
        if (sq_(c)) {
            if (p4 == 0) {
                n = QC(1);
                if (p3 == 0) n = n - 1;
                else {
                    if (n == 0) QC(5) = (int32_t)pos;
                    if (p5 != 0) QC(3) += 1;
                    n = n + 1;
                }
                QC(1) = n;
            } else QC(2) += 1;
        }
    } else {
        n = QC(0);
        if (p3 == 0) n = n - 1;
        else {
            if (n == 0) QC(6) = (int32_t)pos;
            n = n + 1;
        }
        QC(0) = n;
    }
}
static void quote_norm(sstate *st)   /* 1801628a0 */
{
    if (QC(0) < 0) { QC(0) = 0; QC(6) = 0; }
    if (QC(4) < 0) { QC(4) = 0; QC(7) = 0; }
    if (QC(1) < 0) { QC(1) = 0; QC(2) = 0; QC(3) = 0; QC(5) = 0; }
}
static int quote_open(sstate *st) { return 0 < QC(0) || 0 < QC(4); }   /* 180161028 */
static int quote_closesq(sstate *st, int tok)   /* 180160f40 */
{
    int32_t n = SI(tok + 0x10), s = SI(tok + 0xc);
    if (sq_(TXT[(uint32_t)(n - 1 + s)])) {
        int p4 = 0;
        if (1 < n && ((TXT[(uint32_t)(n - 2 + s)] - 0x53) & 0xffdf) == 0) p4 = 1;
        quote_count(st, tok, 0, p4, 0);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ save / restore (180162738 / 180161e84) */
static void scan_copy(sstate *st, int dst, int src)   /* 180068f88: ptr, len, start, pos, tok */
{
    int k;
    for (k = 0; k < 5; k++) st->w[(dst >> 2) + k] = st->w[(src >> 2) + k];
    tok_copy(st, dst + 0x18, src + 0x18);
}
static void ss_save(sstate *st, ssave *sv)
{
    int k;
    scan_copy(st, 0x40, 0x08);
    SI(0x78) = 1;
    sv->w[0] = 1;
    sv->w[1] = W(0x2fc);
    sv->w[2] = W(0x300);
    sv->w[3] = W(0x304);
    for (k = 0; k < 8; k++) {
        sv->w[0x72 + k] = st->w[(0x98 >> 2) + k];
        sv->w[0x7a + k] = st->w[(0xb8 >> 2) + k];
        sv->w[0x82 + k] = st->w[(0xd8 >> 2) + k];
    }
    for (k = 0; k < 0x66; k++) sv->w[4 + k] = st->w[(0x100 >> 2) + k];
    for (k = 0; k < 8; k++) sv->w[0x6a + k] = st->w[(0x298 >> 2) + k];
}
static void ss_restore(sstate *st, ssave *sv)
{
    int k;
    if (sv->w[0] == 0) return;
    scan_copy(st, 0x08, 0x40);
    sv->w[0] = 0;
    W(0x2fc) = sv->w[1];
    W(0x300) = sv->w[2];
    W(0x304) = sv->w[3];
    for (k = 0; k < 0x66; k++) st->w[(0x100 >> 2) + k] = sv->w[4 + k];
    for (k = 0; k < 8; k++) st->w[(0x298 >> 2) + k] = sv->w[0x6a + k];
    for (k = 0; k < 8; k++) {
        st->w[(0x98 >> 2) + k] = sv->w[0x72 + k];
        st->w[(0xb8 >> 2) + k] = sv->w[0x7a + k];
        st->w[(0xd8 >> 2) + k] = sv->w[0x82 + k];
    }
    W(0xf8) = W(0xa0);
}

/* ------------------------------------------------------------------ states */
static int32_t st_m1(sstate *st)   /* 1801623e0 */
{
    ssave sv;
    int32_t sub = 0, saved = 0, t, tmp;
    memset(&sv, 0, sizeof sv);
top:
    if (sub < 0) return sub;
    if (tok_atend(st, 0x98)) return -7;
    t = SI(0xf8);
    if (t == 0x13) {
        if (saved != 0 && brk_top(st, &tmp)) {
            ss_restore(st, &sv);
            return -2;
        }
        return -6;
    }
    if (sub == 0) {
        if (t != 1 && t != 2) {
            if (t == 10) {
                brk_push(st, 0x98);
                sub = 10;
            } else {
                if (t != 0xd) goto l685;
                quote_count(st, 0x98, 1, 0, 0);
            }
        }
    } else {
        if (sub == 10) {
            if (t == 1 || t == 2) goto l6c7;
            if (t == 10 || t == 0xb) {
            l659:
                brk_push(st, 0x98);
                goto l6c7;
            }
        l685:
            sub = 0x32;
            goto top;
        }
        if (sub == 0x32) {
            if (t == 1 || t == 2) goto l6c7;
            if (t == 3) {
                int b = tok_2aa4(st, 0x98);
                sub = -2;
                if (!b && (SI(0x2bc) == 0 || SI(0xa8) == 1)) {
                    ss_save(st, &sv);
                    ss_next(st);
                    sub = 0x3c;
                    saved = (int32_t)sv.w[0];
                }
                goto top;
            }
            sub = -2;
            if (t != 4 || (SI(0x2bc) != 0 && 2 < W(0xa8))) goto top;
        l51a:
            sub = 0x46;
        } else {
            if (sub != 0x3c) {
                if (sub == 0x46) {
                    if (t == 1 || t == 2) {
                        W(0x2c4) = W(0xa4);
                        sub = -2;
                    } else {
                        if (t == 0xb) goto l659;
                        if (t != 0xc || TXT[W(0xa4)] != 0x2e) goto l4fd;
                    }
                    goto l6c7;
                }
                if (sub == 0x50) {
                    sub = -2;
                    if (saved != 0) {
                        ss_restore(st, &sv);
                        saved = (int32_t)sv.w[0];
                    }
                } else if (sub == 0x5a) {
                    if (t == 0xb) {
                    l504:
                        brk_push(st, 0x98);
                        goto l51a;
                    }
                l4fd:
                    sub = 0x50;
                }
                goto top;
            }
            if (t != 1 && t != 2) {
                if (t != 0xb) {
                    if (t != 0xc || TXT[W(0xa4)] != 0x2e) goto l4fd;
                    goto l51a;
                }
                goto l504;
            }
            sub = 0x5a;
        }
    }
l6c7:
    ss_next(st);
    goto top;
}

static int32_t st_m2(sstate *st)   /* 180064630 */
{
    const zf1_ss *m = st->m;
    uint32_t u17 = 0;
    int32_t sub, nx = 0, t;
    int b;
    goto l676;
l673:
    nx = 0;
l676:
    sub = nx;
    nx = -7;
    if (sub < 0) return sub;
    if (tok_atend(st, 0x98)) return -7;
    if (sub < 0x1a) {
        if (sub == 0x19) {
        l6ee:
            t = SI(0xf8);
            nx = sub;
            if (t == 1 || t == 2) goto adv;
            if (t == 3) {
                if (list_find(&m->list[L_CONJ], TXT + W(0xa4), W(0xa8)) >= 0) ss_next(st);
            }
            nx = -1;
            goto noadv;
        }
        if (sub != 0) {
            if (sub == 5) {
                uint32_t u2 = W(0xe0);
                int32_t i3 = 0, i14;
                int u15 = tok_isnum(st, 0x98);
                if (u2 < 2 || u2 == 10) {
                    quote_count(st, 0xb8, 1, 0, u15);
                    u17 = W(0xa4);
                } else {
                    if (W(0xc4) == u17) {
                        u17 = W(0xa4);
                        i14 = 1;
                    } else {
                        i14 = 0;
                        if (SI(0xc8) != 0 && sq_(TXT[W(0xc4)]) && SI(0xa8) != 0) {
                            zf_char s9 = TXT[W(0xa4)];
                            zf_char u1 = (zf_char)((s9 == 0 || (zf_char)(s9 + 0x1800) < 2) ? 0 : 1);
                            if (c_alnum(u1) != 0) i3 = 1;
                        }
                    }
                    quote_count(st, 0xb8, i14, i3, u15);
                }
                quote_norm(st);
                nx = 0;
                goto noadv;
            }
            if (sub == 10) {
                if (SI(0xf8) == 0xc && TXT[W(0xa4)] == 0x2e) {
                    zf_char c0, want;
                    if (SI(0xe0) == 0x1b) {
                        c0 = TXT[W(0xe4)];
                        want = 0xb0;
                    } else {
                        nx = 0x1e;
                        if (SI(0xe0) != 0xf) goto adv;
                        c0 = TXT[W(0xe4)];
                        if (c0 == 0xba) goto l7db;
                        want = 0x2da;
                    }
                    b = c0 == want;
                    nx = 0x1e;
                    if (b) {
                    l7db:
                        nx = -3;
                    }
                    goto adv;
                }
                goto l673;
            }
            if (sub == 0xf) {
                if (SI(0xf8) == 0xc && TXT[W(0xa4)] == 0x2e) {
                    if (tok_initials(st, 0xb8)) ss_next(st);
                    goto l673;
                }
                goto l673;
            }
            nx = sub;
            if (sub == 0x14) {
                if (SI(0xf8) == 1 || SI(0xf8) == 2) {
                    sub = 0x19;
                    ss_next(st);
                    goto l6ee;
                }
                goto l673;
            }
            goto l676;
        }
        /* sub == 0 */
        t = SI(0xf8);
        if (10 < t) {
            if (t == 0xb) {
                int32_t r = 0;
                if ((SI(0xfc) == 0xc || SI(0xfc) == 0x13) && brk_top(st, &r) && SI(0x2c4) <= r) {
                    sub = -5;
                    SI(0x2b8) = 1;
                }
                goto le09;
            }
            if (t == 0xc) {
            lc7b:
                SI(0x310) = 1;
                goto l7db;
            }
            if (t == 0xd) nx = 5;
            else if (t == 0xe) {
                if (SI(0x2bc) == 0) {
                    nx = sub;
                    if (TXT[W(0xa4)] == 0x3a && !tok_isnum(st, 0xb8) && !tok_isnum(st, 0xd8)) {
                        SI(0x2bc) = 1;
                    ld7d:
                        nx = 0x14;
                    }
                } else {
                    if (TXT[W(0xa4)] != 0x3a) goto ld7d;
                    nx = sub;
                    if (!tok_isnum(st, 0xb8) && !tok_isnum(st, 0xd8)) goto ld7d;
                }
            } else {
                if (t == 0x10 || t == 0x11) goto l676;
                if (t == 0x12) goto l96c;
                nx = sub;
                if (t == 0x13) goto lc7b;
            }
        } else {
            if (t != 10) {
                if (t == 1 || t == 2) nx = 0x5a;
                else if (t == 3 || t == 4 || t == 5 || t == 6 || t == 7) {
                    SI(0x310) = 0;
                    SI(0x2b8) = 0;
                    if (quote_closesq(st, 0x98)) quote_norm(st);
                    t = SI(0xf8);
                    if (t == 3) {
                        if (W(0xa8) == 1) nx = 10;
                        else {
                            nx = sub;
                            if (W(0xa8) & 1) nx = 0xf;
                        }
                    } else if (t == 4) {
                        goto l7db;   /* LANG 9: not one of the special languages */
                    } else {
                        if (t == 5) {
                            if ((tok_eq(st, 0x98, "PS.") || tok_eq(st, 0x98, "P.S.")) && (nx = sub, SI(0xc0) == 0))
                                goto adv;
                        } else if (t != 6) {
                            nx = sub;
                            if (t != 7) goto adv;
                            if ((tok_eq(st, 0x98, "Dr.") || tok_eq(st, 0x98, "St.")) && SI(0xe0) != 0 &&
                                c_upper(TXT[W(0xe4)]) != 0)
                                SI(0xa0) = 5;
                        }
                        nx = 0x1e;
                    }
                } else if (t == 8 || (nx = sub, t == 9)) {
                l96c:
                    nx = 0x50;
                }
                goto adv;
            }
        le09:
            brk_push(st, 0x98);
            SI(0x2bc) = 0;
            nx = sub;
        }
        goto adv;
    }
    if (sub == 0x1e) {
        t = SI(0xf8);
        if (SI(0xfc) == 7) {
            if (t != 1 || W(0xa8) < 2) goto l673;
        } else {
            SI(0x2c0) = 1;
            if (t == 1 || t == 2) {
                if (W(0xa8) < 2) {
                    nx = 0x28;
                    goto adv;
                }
            } else if (t == 0xb) {
                if (SI(0xe0) == 10) goto l673;
            } else if (t != 0xc) {
                if (t == 0xd) {
                    zf_char c = TXT[W(0xa4)];
                    if (dq_(c)) goto l258;
                    b = c == 0xbb;
                } else {
                    if (t == 0x10 || t == 0x11) {
                        if (SI(0xe0) != 0 || tok_eq(st, 0xb8, "No.")) goto l212;
                        goto noadv;
                    }
                    b = t == 0x12;
                }
                if (!b) goto l673;
            }
        }
    l258:
        SI(0x310) = 1;
        nx = -4;
        goto l676;
    }
    if (sub == 0x28) {
        t = SI(0xf8);
        if ((uint32_t)(t - 0xb) < 2) goto l258;
        if (1 < (uint32_t)(t - 0x10)) {
            if (SI(0xe0) == 6) {
                if (!tok_isnum(st, 0x98)) {
                    b = c_lower(TXT[W(0xa4)]) == 0;
                    goto lfc1;
                }
            } else {
                if (is_numpfx(m, TXT + W(0xe4), W(0xe8))) {
                    zf_char c0 = TXT[W(0xa4)];
                    if (!tok_isnum(st, 0x98) && !is_digitw(m, TXT + W(0xa4), W(0xa8)) &&
                        (W(0xa8) != 1 || c_alpha(c0) == 0) && !tok_roman(st, 0x98) && c_lower(c0) == 0)
                        SI(0x310) = 1;
                    else
                        nx = 0;
                    goto noadv;
                }
                if (c_upper(TXT[W(0xa4)]) != 0) {
                    if (list_find(&m->list[L_AMBIG], TXT + W(0xe4), W(0xe8)) < 0) {
                        uint32_t u11 = W(0xa4) + W(0xa8);
                        if (!is_capstart(m, TXT + W(0xa4), W(0xa8)) || W(0x2f0) <= u11 || TXT[u11] == 0x2e)
                            goto lfc7;
                    }
                l212:
                    SI(0x310) = 1;
                    goto noadv;
                }
            }
        lfc7:
            nx = 0;
            goto noadv;
        }
        goto l676;
    }
    if (sub == 0x3c) {
        t = SI(0xf8);
        if (t == 3 || t == 4 || t == 5 || t == 6 || t == 7) {
            if (c_lower(TXT[W(0xa4)]) == 0 || tok_isnum(st, 0x98) || 1 < W(0xc8)) goto l212;
            b = SI(0xe0) == 0xc;
        lfc1:
            if (b) goto l212;
            goto lfc7;
        }
        if (t == 10 || t == 0xb || t == 0xc) goto l258;
        if (t != 0xd) goto l673;
        goto l676;
    }
    if (sub != 0x50) {
        nx = sub;
        if (sub == 0x5a) goto l673;
        goto l676;
    }
    t = SI(0xf8);
    SI(0x310) = 1;
    if (t == 1 || t == 2) {
        nx = 0x3c;
        if (((uint32_t)(SI(0xe0) - 8) & 0xfffffffbu) == 0) SI(0xc0) = 0xc;
    } else {
        nx = sub;
        if (t != 8 && t != 9) {
            if (t != 0xc) {
                if (t == 0xd) goto adv;
                goto l673;
            }
            if (SI(0xc0) != 8 || TXT[W(0xa4)] != 0x2e) goto l673;
        }
    }
adv:
    ss_next(st);
noadv:
    goto l676;
}

static int32_t st_m3(sstate *st)   /* 180161a1c */
{
    int32_t sub = 0, t, tmp;
    SI(0x310) = 1;
    for (;;) {
        for (;;) {
            if (sub < 0) return sub;
            if (tok_atend(st, 0x98)) return -7;
            t = SI(0xf8);
            if (t == 0x13) return -6;
            if (sub != 0) break;
            sub = -4;
            if (t == 1 || t == 2) {
                sub = -7;
                if (1 < W(0xa8)) goto d20;
                sub = -4;
                if (TXT[W(0xc4)] != 0x2e && SI(0xfc) != 4) {
                    sub = 10;
                    if (t != 1 || SI(0xfc) != 0x13) goto d20;
                    sub = -7;
                }
            } else if (t == 0xb) {
                SI(0x2b8) = 1;
                sub = -4;
                if (!brk_top(st, &tmp)) continue;
                brk_push(st, 0x98);
                SI(0x2b8) = 0;
                ss_next(st);
                sub = -4;
            } else if (t != 0xc) {
                if (t == 0xd) {
                    sub = -4;
                    if (sq_(TXT[W(0xa4)])) {
                        quote_count(st, 0x98, 0, 0, 0);
                        quote_norm(st);
                        ss_next(st);
                        sub = -4;
                        continue;
                    }
                } else if (t != 0x12 && (sub = -2, SI(0xfc) == 0x13)) {
                    sub = -7;
                }
            }
        }
        if (sub != 10) continue;
        if (t != 3 && t != 4 && t != 5 && t != 6 && t != 7 && t != 10) {
            if (t == 0xc) {
                sub = 0;
            d20:
                ss_next(st);
            } else {
            b4f:
                if (!quote_open(st)) {
                    sub = -7;
                    if (SI(0x2b8) == 0 && brk_top(st, &tmp)) sub = -5;
                } else sub = -4;
            }
            continue;
        }
        goto b4f;   /* LANG 9: neither the lang-10 nor the lang-0xc rule applies */
    }
}

static int32_t st_m4(sstate *st)   /* 180161ff8 */
{
    int32_t i2 = SI(0xfc), sub = 0, t, tmp;
    int32_t u8;
l015:
    u8 = 0x14;
l01b:
    if (sub < 0) return sub;
    if (tok_atend(st, 0x98)) return -7;
    t = SI(0xf8);
    if (t == 0x13) return -6;
    if (sub != 0) goto l04d;
    sub = quote_open(st) ? 0x14 : 10;
    goto l06f;
l04d:
    if (sub != 10 && sub != u8) goto l01b;
l06f:
    if (t != 1) {
        if (t == 2) goto l222;
        if (t == 3) {
            if (i2 == 5 && SI(0xfc) == 1 && SI(0xc8) == 1) {
                if (c_lower(TXT[W(0xa4)]) != 0) {
                    sub = -2;
                    goto l015;
                }
                u8 = 0x14;
            }
            if (SI(0xfc) == 0xd || (sub == 10 && SI(0x2b8) == 0 && (SI(0xfc) == 0xb || brk_top(st, &tmp)))) {
                sub = -5;
                goto l01b;
            }
        } else {
            if (t == 0xb) {
                int r = brk_top(st, &tmp);
                brk_push(st, 0x98);
                if (!r) SI(0x2b8) = 1;
                goto l222;
            }
            if (t == 0xc) goto l222;
            if (t == 0xd) {
                quote_count(st, 0x98, 0, 0, 0);
                quote_norm(st);
                if (dq_(TXT[W(0xa4)])) {
                    int bb = sub == 10;
                    sub = 0;
                    if (bb) sub = 0x14;
                }
                goto l222;
            }
            if (t == 0xe) {
                sub = -2;
                goto l01b;
            }
            if (t == 0x12) goto l222;
        }
        sub = -7;
        goto l01b;
    }
    if (SI(0x2b8) != 0 || SI(0xa8) != 1 || SI(0xfc) != 0xd) {
        if (sub != 10) goto l222;
        if (SI(0xa8) != 1 || ((((uint32_t)(SI(0xfc) - 0xb)) & 0xfffffffdu) != 0 && !brk_top(st, &tmp))) {
            sub = -7;
        l222:
            ss_next(st);
            goto l015;
        }
    }
    sub = -5;
    goto l222;
}

static int32_t st_m5(sstate *st)   /* 18016225c */
{
    int32_t sub = 0, i6, t;
    for (;;) {
        i6 = sub;
        if (i6 < 0) return i6;
        if (tok_atend(st, 0x98)) return -7;
        t = SI(0xf8);
        if (t == 0x13) return -6;
        sub = -7;
        if (t < 0xb) goto l2b1;
        if (t == 0xb) {
            sub = -4;
            continue;
        }
        if (t == 0xd) {
            sub = (SI(0xfc) != 1) ? -2 : -7;
            continue;
        }
        if (t == 0xf || t == 0x1b || t == 0x1c || t == 0x1d || t == 0x20) {
            if (SI(0x2c0) == 0) continue;
            SI(0x2c0) = 0;
        }
        goto l37d;
    l2b1:
        if (t == 10) continue;
        if (t == 1 || t == 2) {
            ss_next(st);
            sub = i6;
            continue;
        }
        if (t != 3) {
            if (t == 4) {
                if (SI(0xa8) == 2 || SI(0x310) != 0) continue;
            } else if (t != 5 && t != 6 && t != 7) goto l37d;
        }
        if (c_upper(TXT[W(0xa4)]) != 0 || (W(0xac) & 0x40) != 0 ||
            (SI(0xfc) == 1 && (((uint32_t)(SI(0xe0) - 0xb)) & 0xfffffffdu) == 0 && tok_isnum(st, 0x98)))
            continue;
    l37d:
        sub = -2;
    }
}

static int32_t st_m6(sstate *st)   /* 180161e00 */
{
    int32_t sub = 0, t;
    for (;;) {
        if (sub < 0) return sub;
        if (tok_atend(st, 0x98)) return -7;
        t = SI(0xf8);
        if (t != 1 && t != 2) {
            if (t == 0xb) sub = -5;
            else if (t == 0xc) sub = -3;
            else if (t != 0x13) {
                SI(0x310) = 1;
                sub = -7;
                continue;
            }
        }
        ss_next(st);
    }
}

static void st_finish(sstate *st)   /* 1800686e0 (the +0x2f8 trailing-blank count is not needed) */
{
    uint32_t u1, u6;
    if (SI(0xf8) == 0x10) ss_next(st);
    u1 = W(0xa4);
    W(0x2e8) = u1;
    u6 = u1;
    if (SI(0xb0) != 0 && u1 == 0) {
        u6 = W(0xa8);
        W(0x2e8) = u6;
    }
    for (;;) {
        W(0x2ec) = u6;
        if (u6 == 0 || (!ws_(TXT[u6 - 1]) && !nl_(TXT[u6 - 1]))) break;
        u6 = u6 - 1;
    }
    if (!tok_isnul(st, 0x98) && (SI(0xb0) == 0 || u1 != 0)) {
        int32_t t = SI(0xf8);
        if (t != 1 && t != 2) {
            if (t == 3 || t == 4 || t == 5 || t == 6 || t == 7) {
                SI(0x300) -= SI(0xa8);
                SI(0x2fc) -= 1;
            } else if (t == 0x10) return;
            SI(0x304) -= SI(0xa8);
        }
    }
}

static void ss_reset(sstate *st)   /* 180073a18 */
{
    W(0x10) = W(0x2f0);
    W(0x14) = W(0x18) = 0;
    W(0x20) = W(0x24) = W(0x28) = W(0x2c) = W(0x30) = W(0x34) = W(0x38) = 0;
    SI(0x104) = -1;
    W(0x100) = 0;
    W(0x298) = W(0x29c) = W(0x2a0) = W(0x2a4) = W(0x2a8) = W(0x2ac) = W(0x2b0) = W(0x2b4) = W(0x2b8) = 0;
    W(0x2fc) = W(0x300) = W(0x304) = W(0x308) = W(0x30c) = W(0x310) = W(0x314) = 0;
    W(0x2d4) = W(0x2d8) = 0;
    W(0x2bc) = W(0x2c0) = 0;
    W(0x2c4) = 0;
    W(0x98) = W(0x9c) = W(0xa0) = W(0xa4) = W(0xa8) = W(0xac) = W(0xb0) = 0;
    ss_next(st);
}

/* ------------------------------------------------------------------ public */
int zf1_ss_run(const zf1_ss *s, const zf_char *text, int len, int *consumed, int *hard_end)
{
    sstate *st;
    uint32_t n4, skip = 0, u4;
    int hard = 0;
    uint32_t L = (uint32_t)(len < 0 ? 0 : len);
    *consumed = 0;
    *hard_end = 0;
    if (!s || !s->ok || !text) return -1;
    n4 = L > 499 ? 500 : L;
    if (n4 != 0) {
        const zf_char *p = text;
        do {
            if (!ws_(*p) && !nl_(*p)) break;
            skip++;
            p++;
        } while (skip < n4);
        if (499 < skip) {
            *consumed = 500;
            return 0;
        }
    }
    st = (sstate *)calloc(1, sizeof *st);
    if (!st) return -2;
    st->m = s;
    SI(0x90) = 1;
    SI(0x104) = -1;
    /* 180073970 run */
    st->text = text + skip;
    W(0x2f0) = L - skip;
    SI(0) = -1;
    ss_reset(st);
    for (;;) {
        int32_t k = SI(0);
        if (k == -7) {
            SI(0) = 0;
            st_finish(st);
            break;
        }
        if (k == -6) k = st_m6(st);
        else if (k == -5) k = st_m5(st);
        else if (k == -4) k = st_m4(st);
        else if (k == -3) k = st_m3(st);
        else if (k == -2) k = st_m2(st);
        else if (k == -1) k = st_m1(st);
        else { k = -7; }   /* the engine would spin; never happens */
        SI(0) = k;
    }
    u4 = W(0x2ec) + skip;
    *consumed = (int)u4;
    if (u4 == 0) {
        if (L != 0) *consumed = 1;
    } else if (500 < u4) {
        int32_t i9 = (int32_t)skip;
        if (skip == 0) {
            int32_t k = 499;
            i9 = 499;
            do {
                if (iswspace_(text[k]) || nl_(text[k])) break;
                i9--;
                k--;
            } while (-1 < k);
            k = i9;
            if (-1 < i9) {
                do {
                    if (!iswspace_(text[k]) && !nl_(text[k])) break;
                    i9--;
                    k--;
                } while (-1 < k);
            }
            i9 = (i9 < 0) ? 500 : i9 + 1;
        }
        *consumed = i9;
        free(st);
        return 0;
    }
    if (SI(0x310) != 0 || SI(0x314) != 0) hard = 1;
    *hard_end = hard;
    free(st);
    return 0;
}

/* 1800b682c: 0 for say-as "address" / "name" (not sentence-split), 1 otherwise */
static int sayas_split(const zf_char *c)
{
    if (!c || !*c) return 1;
    if (!zf_strcmp_a(c, "address") || !zf_strcmp_a(c, "name")) return 0;
    return 1;
}

/* 1800624e0 (the enumerator's piece loop; its hard-end flag is the separator's vt+0x18, reset at entry) */
int zf1_ss_next(const zf1_ss *s, const zf1_ssfrag *f, int nfrag, zf1_sspos *cur, zf1_sspos *start, zf1_sspos *end)
{
    int fi = cur->frag, pos = cur->pos, endp, done = 0, exh = 0, hard = 0;
    uint32_t total = 0, u10 = 0;
    if (fi < 0 || fi >= nfrag) return 0;
    start->frag = fi;
    start->pos = pos;
    endp = f[fi].len;
    for (;;) {
        int prev = pos, act;
        if (fi < 0 || done) {
            cur->frag = fi;
            cur->pos = pos;
            end->frag = fi;
            end->pos = pos;
            return 1;
        }
        act = f[fi].action;
        if (act > 9 || !((0x211u >> act) & 1)) goto l707;
        if (!f[fi].text) return (int)0x8000FFFFu;
        if (pos < endp) {
            uint32_t rem = (uint32_t)(endp - pos), u14 = rem;
            if (!f[fi].sayas || sayas_split(f[fi].sayas) || 499 < rem) {
                if (act == 9) {
                    if (499 < rem) return (int)0x80048028u;
                } else {
                    int c, h;
                    zf1_ss_run(s, f[fi].text + pos, (int)rem, &c, &h);
                    u14 = (uint32_t)c;
                    hard = h;
                }
            }
            u10 = total + u14;
            if (hard || 499 < rem) done = 1;
            pos += (int)u14;
            if (500 < u10) {
                u10 -= u14;
                done = 1;
                pos = prev;
            }
        }
        if (pos >= 0 && endp >= 0 && endp <= pos) exh = 1;
        if (!done && exh && u10 != 0 && fi + 1 < nfrag && (f[fi + 1].action == 6 || f[fi + 1].action == 7)) done = 1;
        total = u10;
        if (!exh) continue;
    l707:
        exh = 0;
        if (done) {
            if (fi + 1 < nfrag) {
                if (f[fi + 1].action == 6 || f[fi + 1].action == 7) fi = fi + 1;
                continue;
            }
            fi = -1;
        } else {
            fi = fi + 1;
            if (fi < nfrag) {
                pos = 0;
                endp = f[fi].len;
                continue;
            }
            fi = -1;
        }
        pos = -1;
        endp = -1;
    }
}

int zf1_ss_init(zf1_ss *s, const zf1_dat *d)
{
    size_t sz;
    const uint8_t *r = zf1_dat_get(d, 0xe67ab014u, 0x1df28780u, &sz), *pool;
    uint32_t cnt[12], tot = 0, k, i, j = 0;
    memset(s, 0, sizeof *s);
    if (!r || sz < 4 + 48 || zf_rd32(r) != 12) return -1;
    for (k = 0; k < 12; k++) { cnt[k] = zf_rd32(r + 4 + 4 * k); tot += cnt[k]; }
    pool = r + 4 + 48 + 4 * (size_t)tot;
    for (k = 0; k < 12; k++) {
        s->list[k].n = (int)cnt[k];
        s->list[k].s = (zf_char **)calloc(cnt[k] ? cnt[k] : 1, sizeof(zf_char *));
        for (i = 0; i < cnt[k]; i++, j++) {
            const uint8_t *e = pool + zf_rd32(r + 52 + 4 * (size_t)j);
            size_t n = 0;
            while (e + 2 * n + 1 < r + sz && zf_rd16(e + 2 * n)) n++;
            s->list[k].s[i] = (zf_char *)malloc((n + 1) * sizeof(zf_char));
            for (size_t q = 0; q < n; q++) s->list[k].s[i][q] = zf_rd16(e + 2 * q);
            s->list[k].s[i][n] = 0;
        }
    }
    r = zf1_dat_get(d, 0xf6e4f50au, 0x80b9a5a3u, &sz);
    if (r && sz >= 4) {
        s->ctab.n = (int)zf_rd32(r);
        s->ctab.rec = r + 4;
    }
    s->ok = 1;
    return 0;
}

void zf1_ss_free(zf1_ss *s)
{
    int k, i;
    for (k = 0; k < 12; k++) {
        for (i = 0; i < s->list[k].n; i++) free(s->list[k].s[i]);
        free(s->list[k].s);
    }
    memset(s, 0, sizeof *s);
}

/* exported for the enumerator helpers (zf1_engine.c): engine FUN_180072d30 blank/control test */
int zf1_ss_ws(zf_char c) { return ws_(c); }
