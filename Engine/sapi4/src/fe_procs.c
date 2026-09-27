// The rule procedures: what a rule's output 0xfe,n runs instead of a replacement string (fe_vm.c,
// vm_apply). Each instance has a table of them; most are the same thin wrapper that runs another rule
// set of the same machine over the segment (vm_main), the rest do tape work of their own.
#include "fe_vm.h"
#include "fe_lex.h"
#include <stddef.h>
#include <string.h>
#include "vcrt.h"

enum { PROC_GUEST, PROC_MAIN, PROC_FALSE, PROC_FILL_FORWARD, PROC_CLEAR4, PROC_QUOTES, PROC_CHOOSE,
       PROC_NUMBER_DASHES, PROC_BLANK3, PROC_COPY_1_0, PROC_COPY_3_0, PROC_COPY_0_123, PROC_LEX, PROC_LEXMAIN,
       PROC_TAGS, PROC_PAUSE_VERB, PROC_PAUSE_B, PROC_COPY_3_0_B, PROC_CLASS_B, PROC_LOOKUP_LOOP, PROC_PAREN_OR };
typedef struct RuleProc {
    uint32_t addr;          // the original (for PROC_GUEST: still called there)
    uint8_t kind;
    int8_t mode;            // PROC_MAIN: the rule set; PROC_LEX: the dictionary (lex_dicts)
    int8_t res;             // PROC_MAIN: -1 = its result, else this constant in AL
} RuleProc;
static const RuleProc rule_procs[3][62] = {
    { // instance A
        [3] = { 0x6368b28b, PROC_FILL_FORWARD, 0, 0 },
        [4] = { 0x6368b2e0, PROC_CLEAR4, 0, 0 },
        [5] = { 0x6368b6df, PROC_MAIN, 5, -1 },
        [6] = { 0x6368b6f5, PROC_MAIN, 6, 1 },
        [7] = { 0x6368b70d, PROC_MAIN, 7, 1 },
        [8] = { 0x6368b725, PROC_MAIN, 8, 1 },
        [9] = { 0x6368b73d, PROC_MAIN, 9, 1 },
        [10] = { 0x6368b755, PROC_MAIN, 10, -1 },
        [11] = { 0x6368b383, PROC_TAGS, 0, 0 },
        [12] = { 0x6368b305, PROC_QUOTES, 0, 0 },
        [13] = { 0x6368b6bc, PROC_CHOOSE, 0, 0 },
        [14] = { 0x6368b76b, PROC_MAIN, 14, 1 },
        [15] = { 0x6368b783, PROC_MAIN, 15, 1 },
        [16] = { 0x6368b79b, PROC_MAIN, 16, -1 },
    },
    { // instance B
        [3] = { 0x636908d3, PROC_MAIN, 3, -1 },
        [4] = { 0x6369048f, PROC_PAUSE_VERB, 0, 0 },
        [5] = { 0x63690633, PROC_PAUSE_B, 0, 0 },
        [6] = { 0x636908e9, PROC_MAIN, 6, 1 },
        [7] = { 0x63690215, PROC_COPY_3_0_B, 0, 0 },
        [8] = { 0x6369024b, PROC_CLASS_B, 0, 0 },
        [9] = { 0x63690901, PROC_MAIN, 9, -1 },
        [10] = { 0x63690917, PROC_MAIN, 10, -1 },
        [11] = { 0x6369092d, PROC_MAIN, 11, 1 },
        [12] = { 0x63690945, PROC_MAIN, 12, -1 },
        [13] = { 0x6369095b, PROC_MAIN, 13, 1 },
        [14] = { 0x63690973, PROC_MAIN, 14, 1 },
        [15] = { 0x6369098b, PROC_MAIN, 15, -1 },
        [16] = { 0x636909a1, PROC_MAIN, 16, -1 },
    },
    { // instance C
        [3] = { 0x636939b0, PROC_NUMBER_DASHES, 0, 0 },
        [4] = { 0x63693984, PROC_BLANK3, 0, 0 },
        [5] = { 0x63693b64, PROC_MAIN, 5, -1 },
        [6] = { 0x63693b7a, PROC_MAIN, 6, -1 },
        [7] = { 0x63693b90, PROC_MAIN, 7, -1 },
        [8] = { 0x63693ba6, PROC_MAIN, 8, -1 },
        [9] = { 0x63693bbc, PROC_MAIN, 9, -1 },
        [10] = { 0x63693bd2, PROC_MAIN, 10, -1 },
        [11] = { 0x63693be8, PROC_MAIN, 11, -1 },
        [12] = { 0x63693bfe, PROC_MAIN, 12, -1 },
        [13] = { 0x63693c14, PROC_MAIN, 13, -1 },
        [14] = { 0x63693c2a, PROC_MAIN, 14, -1 },
        [15] = { 0x63693c40, PROC_MAIN, 15, -1 },
        [16] = { 0x63693c56, PROC_MAIN, 16, -1 },
        [17] = { 0x63693c6c, PROC_MAIN, 17, -1 },
        [18] = { 0x63693c82, PROC_MAIN, 18, -1 },
        [19] = { 0x63693c98, PROC_MAIN, 19, -1 },
        [20] = { 0x63693cae, PROC_MAIN, 20, -1 },
        [21] = { 0x6369a4ea, PROC_LEX, 0, 0 },
        [22] = { 0x6369986b, PROC_LEXMAIN, 0, 0 },
        [23] = { 0x636938b9, PROC_COPY_1_0, 0, 0 },
        [24] = { 0x63693cc4, PROC_MAIN, 24, 0 },
        [25] = { 0x636993f8, PROC_LEX, 6, 0 },
        [26] = { 0x63698f1a, PROC_LEX, 5, 0 },
        [27] = { 0x63698a3e, PROC_LEX, 3, 0 },
        [28] = { 0x63693cdc, PROC_MAIN, 28, -1 },
        [29] = { 0x63693cf2, PROC_MAIN, 29, -1 },
        [30] = { 0x6369382c, PROC_LOOKUP_LOOP, 0, 0 },
        [31] = { 0x63693925, PROC_COPY_0_123, 0, 0 },
        [32] = { 0x63693d08, PROC_MAIN, 32, -1 },
        [33] = { 0x63693d1e, PROC_MAIN, 33, -1 },
        [34] = { 0x63698594, PROC_LEX, 4, 0 },
        [35] = { 0x636980bf, PROC_LEX, 2, 0 },
        [36] = { 0x63697c17, PROC_LEX, 7, 0 },
        [37] = { 0x63693d34, PROC_MAIN, 37, -1 },
        [38] = { 0x63693d4a, PROC_MAIN, 38, -1 },
        [39] = { 0x63693d60, PROC_MAIN, 39, -1 },
        [40] = { 0x63693d76, PROC_MAIN, 40, -1 },
        [41] = { 0x6369397f, PROC_FALSE, 0, 0 },
        [42] = { 0x63693a03, PROC_PAREN_OR, 0, 0 },
        [43] = { 0x63693d8c, PROC_MAIN, 43, -1 },
        [44] = { 0x63693da2, PROC_MAIN, 44, 0 },
        [45] = { 0x63693dba, PROC_MAIN, 45, 0 },
        [46] = { 0x63693dd2, PROC_MAIN, 46, -1 },
        [47] = { 0x63693de8, PROC_MAIN, 47, -1 },
        [48] = { 0x63693dfe, PROC_MAIN, 48, 1 },
        [49] = { 0x63693e16, PROC_MAIN, 49, 0 },
        [50] = { 0x63693e2e, PROC_MAIN, 50, -1 },
        [51] = { 0x63693e44, PROC_MAIN, 51, -1 },
        [52] = { 0x636938ef, PROC_COPY_3_0, 0, 0 },
        [53] = { 0x6369773d, PROC_LEX, 1, 0 },
        [54] = { 0x636972c7, PROC_LEX, 8, 0 },
        [55] = { 0x63693e5a, PROC_MAIN, 55, -1 },
        [56] = { 0x63693e70, PROC_MAIN, 56, -1 },
        [57] = { 0x63693e86, PROC_MAIN, 57, -1 },
        [58] = { 0x63693e9c, PROC_MAIN, 58, 1 },
        [59] = { 0x63693eb4, PROC_MAIN, 59, 1 },
        [60] = { 0x63693ecc, PROC_MAIN, 60, -1 },
        [61] = { 0x63693ee2, PROC_MAIN, 61, -1 },
    },
};

uint32_t vm_proc(int inst, uint8_t n, int16_t *left, int16_t *len, uint32_t prev_eax);
// ---- procedures with tape work of their own

#define TP(inst, k) ((uint8_t *)GP(char, *DLLPTR(char, rule_vm[inst].tape[k])))
// machine A's word object (the block the tapes are part of)
#define WOBJ_A GP(uint8_t, *DLLPTR(uint8_t, 0x63739274))

// MSVCRT's isdigit in the C locale (the originals test _pctype, __mb_cur_max being 1)
static int is_digit8(uint8_t c) { return (vc_ctype(c) & 4) != 0; }
// a sign kind: 1 digit first, 3 '-', 2 anything else
static int sign_kind(uint8_t c) { return is_digit8(c) ? 1 : c == 0x2d ? 3 : 2; }

// @0x6368b468 stdcall: the number on tape 0 at b-1 .. c-1 (a sign or a digit first), clamped to
// +-2700, +3000 if it had no sign, into the int16 at word object + 0x1b88 + 2a
void tag_number(int16_t a, int16_t b, int16_t c) {
    const uint8_t *t = TP(0, 0);
    int32_t kind = sign_kind(t[b - 1]), first = kind == 1 ? b : b + 1;
    int32_t v = 0;
    for (int32_t i = first - 1; i < c; i++) v = v * 10 + t[i] - 0x30;
    if (kind == 3) { v = -v; kind = 2; }
    if (v > 0xa8c) v = 0xa8c;
    else if (v < -0xa8c) v = -0xa8c;
    if (kind == 1) v += 0xbb8;
    int16_t s = (int16_t)v;
    memcpy(WOBJ_A + 0x1b88 + 2 * a, &s, 2);
}

// @0x6368b526 stdcall: at b (after an optional 0x69): a 0x1e starts two numbers (each with an optional
// sign, the first ending at 0x1d, the second at 0x1f), appended as a pair to the word object's table
// (up to 640; the second at least 1) and indexed from slot a; anything else is copied to tape 4
void tag_pair(int16_t a, int16_t b, int16_t c) {
    const uint8_t *t = TP(0, 0);
    int32_t i = b;
    if (t[i - 1] == 0x69) i++;
    uint8_t ch = t[i - 1];
    if (ch != 0x1e) { TP(0, 4)[a - 1] = ch; return; }
    i++;
    int kind1 = sign_kind(t[i - 1]);
    if (kind1 > 1) i++;
    int32_t v1 = 0;
    uint8_t d = t[i - 1];
    do { v1 = v1 * 10 + d - 0x30; i++; d = t[i - 1]; } while (d != 0x1d && i < c);
    if (kind1 == 3) v1 = -v1;
    i++;
    int kind2 = sign_kind(t[i - 1]);
    if (kind2 > 1) i++;
    int32_t v2 = 0;
    d = t[i - 1];
    do { v2 = v2 * 10 + d - 0x30; i++; d = t[i - 1]; } while (d != 0x1f && i < c);
    if (kind2 == 3) v2 = -v2;
    uint8_t *w = WOBJ_A;
    int16_t n;
    memcpy(&n, w + 0x438c, 2);
    if (n >= 0x280) return;
    n++;
    memcpy(w + 0x438c, &n, 2);
    memcpy(w + n * 8 + 0x2f84, &v1, 4);
    if (v2 <= 0) v2 = 1;
    int16_t s2 = (int16_t)v2;
    memcpy(w + n * 8 + 0x2f88, &s2, 2);
    memcpy(w + 0x2588 + 2 * a, &n, 2);
}

// A, 11: the embedded prosody tags of the segment (0x1e ... 0x1f groups, up to a 0x5d)
static uint8_t proc_tags(int16_t *left, int16_t *len) {
    const uint8_t *t = TP(0, 0);
    uint8_t flag = 0;
    int32_t i = *left + 1, start = i;
    uint8_t dl = t[i - 1];
    while (dl != 0x5d) {
        int32_t end = *len;
        if (i >= end) break;
        for (;;) {
            if ((dl == 0x1d || t[i - 1] == 0x5d || t[i - 1] == 0x1e) && !flag) break;
            if (i > end) break;
            if (t[i - 1] == 0x1e) flag = 1;
            dl = t[i];
            i++;
            if (dl == 0x1f) flag = 0;
        }
        if (i > start) {
            uint8_t c = t[start - 1];
            if (c == 0x1e || c == 0x69) tag_pair((int16_t)(*left - 1), (int16_t)start, (int16_t)(i - 1));
            else tag_number((int16_t)(*left - 1), (int16_t)start, (int16_t)(i - 1));
            t = TP(0, 0);
        }
        if (t[i - 1] == 0x1e) i--;
        i++;
        dl = t[i - 1];
        if (dl == 0x1e) flag = 1;
        start = i;
    }
    *len = (int16_t)(*left - 1);
    return 1;
}

// @0x636905f9 stdcall: spaces minus 0x11 marks on tape 0 in [a, b)
int32_t count_words(int16_t a, int16_t b) {
    const uint8_t *t = TP(1, 0);
    int32_t n = 0;
    for (int32_t i = a; i < b; i++) {
        if (t[i] == 0x20) n++;
        else if (t[i] == 0x11) n--;
    }
    return n;
}

static int is_break8(uint8_t c) {           // , . ; ? ! : # and the pause mark 0x19
    return c == 0x2c || c == 0x2e || c == 0x3b || c == 0x3f || c == 0x21 || c == 0x3a || c == 0x23 || c == 0x19;
}
static int is_break10(uint8_t c) { return is_break8(c) || c == 0x28 || c == 0x29; }

// B, 4: a pause (0x19) before a verb (tape 3 'V', tape 2 not 'S') that follows a noun phrase ('N'), when
// both sides of it have at least three words up to the next break
static uint8_t proc_pause_verb(int16_t *left, int16_t *len) {
    for (int16_t di = (int16_t)(*left - 1); di < *len; di++) {
        if (TP(1, 3)[di] != 0x56 || TP(1, 2)[di] == 0x53) continue;
        int16_t si = (int16_t)(di - 1);
        for (;;) {
            uint8_t d = TP(1, 3)[si];
            if (d != 0x20 && d != 0x7e) break;
            if (!(si > *left - 1)) break;
            uint8_t c2 = TP(1, 2)[si];
            if (c2 == 0x50 || c2 == 0x53) break;
            if (is_break8(TP(1, 0)[si])) break;
            si--;
        }
        int16_t np = si;
        int32_t n1 = count_words(si, di);
        int16_t dx = (int16_t)(di + 1);
        while (dx < *len && !is_break8(TP(1, 0)[dx])) dx++;
        int32_t n2 = count_words(di, dx);
        dx = di;
        if (TP(1, 0)[di - 1] == 0x7e) {
            do {
                if (dx <= *left) break;
                dx--;
            } while (TP(1, 0)[dx - 1] == 0x7e);
        }
        uint8_t *p = TP(1, 0) + dx - 1;
        if (*p == 0x20 && TP(1, 3)[np] == 0x4e && (int16_t)n1 >= 3 && (int16_t)n2 >= 3) *p = 0x19;
    }
    return 1;
}

// B, 5: pauses around 'B' words after a 'P' (with a question mark inside a word protected as '%'), and
// a noun ('N') after an 'XV' word loses its marks
static uint8_t proc_pause_b(int16_t *left, int16_t *len) {
    for (int32_t i = *left; i < *len - 1; i++)
        if (TP(1, 0)[i] == 0x3f && TP(1, 0)[i + 1] != 0x20) TP(1, 0)[i] = 0x25;
    for (int16_t si = *left; si < *len; si++) {
        uint8_t flag = 0;
        if (TP(1, 0)[si] != 0x42 || TP(1, 3)[si - 1] != 0x50) continue;
        int32_t lim = *left - 1;
        int16_t di = (int16_t)(si - 2);
        while (di > lim && !is_break10(TP(1, 0)[di])) di--;
        int16_t bx = di;
        if (TP(1, 0)[di + 1] == 0x20) bx = (int16_t)(di + 2);
        int32_t n1 = count_words(bx, si);
        di++;
        while (di < *len && !is_break10(TP(1, 0)[di])) di++;
        int32_t n2 = count_words(si, di);
        int16_t bp = 0;
        for (int16_t dx = (int16_t)(si - 1); dx >= bx && bp < 3; dx--) {
            if (TP(1, 0)[dx] == 0x20) bp++;
            uint8_t a = TP(1, 2)[dx];
            if (a == 0x56 || a == 0x76 || a == 0x52 || a == 0x72 || a == 0x78 || a == 0x58 || a == 0x4d) flag = 1;
        }
        uint8_t *p = TP(1, 0) + si - 2;
        if (*p == 0x20 && !flag && (int16_t)n1 >= 2 && (int16_t)n2 >= 3) {
            *p = 0x19;
            int16_t ax = (int16_t)(si + 1), sp = 0;
            do { if (TP(1, 0)[ax] == 0x20) sp++; ax++; } while (sp < 3);
            si = (int16_t)(ax - 1);
        }
    }
    for (int32_t i = *left; i < *len - 1; i++)
        if (TP(1, 0)[i] == 0x25) TP(1, 0)[i] = 0x3f;
    for (int16_t ax = (int16_t)(*len - 2); ax > *left; ax--) {
        uint8_t *p = TP(1, 3) + ax;
        if (*p != 0x4e) continue;
        int32_t edx = *left;
        if (!(ax - 2 > edx)) continue;
        edx = *left - 1;
        int16_t bp = ax;
        if (ax > edx) {
            int32_t e = ax;
            do {
                uint8_t c = TP(1, 0)[e];
                if (c == 0x20 || c == 0x19) break;
                bp--;
                e = bp;
            } while (e > edx);
        }
        for (;;) {
            bp--;
            if (!(bp > edx)) break;
            uint8_t c = TP(1, 0)[bp];
            if (c == 0x20 || c == 0x19) break;
        }
        bp++;
        if (TP(1, 0)[bp] == 0x47) bp++;
        if (TP(1, 2)[bp] == 0x58 && TP(1, 3)[bp] == 0x56) {
            *p = 0x7e;
            TP(1, 3)[ax - 1] = 0x7e;
        }
    }
    return 1;
}

// B, 7: copy tape 3 to tape 0 over the segment
static uint8_t proc_copy_3_0_b(int16_t *left, int16_t *len) {
    for (int16_t i = (int16_t)(*left - 1); i < *len; i++) TP(1, 0)[i] = TP(1, 3)[i];
    return 1;
}

// B, 8: after the 0x11 and '@' marks on tape 2, a class letter and an 'H' with a digit decide whether
// to run rule set 9 of machine B, and which class the segment's tape 2 gets (the rest becomes filler)
static uint8_t proc_class_b(int16_t *left, int16_t *len) {
    uint8_t *tc = TP(1, 2);
    int16_t cx = (int16_t)(*left - 1), si = *len;
    int16_t saved;
    while (cx < si && tc[cx] != 0x11) cx++;
    while (cx < si && tc[cx] != 0x40) cx++;
    do cx++; while (cx < si && tc[cx] == 0x7e);
    uint8_t bl = tc[cx];
    cx = (int16_t)(cx + 2);
    saved = cx;
    for (;;) {
        if (!(cx < si)) { cx = saved; break; }
        uint8_t d = tc[cx];
        if (d == 0x20 || d == 0x7e) { cx++; continue; }
        saved = cx;
        break;
    }
    if (bl == 0x40) {
        if (tc[cx] != 0x48) return 1;
        const uint8_t *td = TP(1, 3);
        while (td[cx] != 0x26 && td[cx] != 0x20) cx++;
        while (td[cx] == 0x7e) cx++;
        if (cx > saved) proc_copy_3_0_b(&saved, &cx);
        return 1;
    }
    if (tc[cx] == 0x48) {
        do cx++; while (cx < si && tc[cx] == 0x7e);
        uint8_t digit = (uint8_t)(tc[cx] - 0x30);
        int run9 = 0;
        switch (tc[cx]) {
        case 0x30:
            if (bl == 0x31) {
                tc[saved] = 0x7e;
                TP(1, 2)[cx] = 0x7e;
                return 1;
            } else {
                int16_t s = (int16_t)(cx + 1);
                while (s < si) {
                    uint8_t c = tc[s];
                    if (c == bl || (c == 0x76 && bl == 0x56)) break;
                    s++;
                }
                if (s == si) {
                    vm_proc(1, 9, &saved, len, 0);
                    if (bl == 0x32) return 1;
                    tc = TP(1, 2);
                    tc[saved] = (char)bl;
                    for (int16_t a = (int16_t)(saved + 1); a < *len; a++) TP(1, 2)[a] = 0x7e;
                    return 1;
                }
                tc[saved - 1] = bl;
                for (int16_t a = saved; a < *len; a++) TP(1, 2)[a] = 0x7e;
                tc = TP(1, 2);
            }
            break;
        case 0x34: run9 = bl == 0x4a || bl == 0x4e; break;
        case 0x31: case 0x32: case 0x33: case 0x38: run9 = bl == 0x4e; break;
        case 0x35: case 0x36: run9 = bl == 0x4a; break;
        case 0x37: run9 = bl == 0x52; break;
        default: (void)digit; break;
        }
        if (run9) { vm_proc(1, 9, &saved, len, 0); return 1; }
    }
    tc[saved] = bl;
    for (int16_t a = (int16_t)(saved + 1); a < *len; a++) TP(1, 2)[a] = 0x7e;
    return 1;
}

// C, 30: run procedures 57 and 56 alternately until a dictionary lookup (flags of dictionaries 1 and
// 8) succeeds or 56 fails; then 58 if one did, else 59. AL = found.
static uint8_t proc_lookup_loop(int16_t *left, int16_t *len) {
    uint8_t *fa = DLLVAR(uint8_t, 0x63739e6a), *fb = DLLVAR(uint8_t, 0x63739e6b);
    *fa = 0;
    *fb = 0;
    vm_proc(2, 57, left, len, 0);
    uint8_t f = *fb || *fa;
    while (!f) {
        uint8_t r = (uint8_t)vm_proc(2, 56, left, len, 0);
        if (r) vm_proc(2, 57, left, len, 0);
        f = *fb || *fa;
        if (!r) break;
    }
    vm_proc(2, f ? 58 : 59, left, len, 0);
    return f;
}

// C, 42: "x(y)z" becomes "xz or xyz" (built on tape 1, copied back to tape 0)
static uint8_t proc_paren_or(int16_t *left, int16_t *len) {
    int16_t ax = (int16_t)(*left - 1), si = ax;
    uint8_t dl = TP(2, 0)[ax];
    while (dl != 0x28) { TP(2, 1)[ax] = (char)dl; ax++; si++; dl = TP(2, 0)[si]; }
    { int16_t e; do { e = si; si++; } while (TP(2, 0)[e] != 0x29); }
    while (si < *len) { TP(2, 1)[ax] = TP(2, 0)[si]; ax++; si++; }
    TP(2, 1)[ax++] = 0x20;
    TP(2, 1)[ax++] = 0x6f;
    TP(2, 1)[ax++] = 0x72;
    TP(2, 1)[ax++] = 0x20;
    int16_t cx = (int16_t)(*left - 1);
    dl = TP(2, 0)[cx];
    while (dl != 0x28) { TP(2, 1)[ax++] = (char)dl; cx++; dl = TP(2, 0)[cx]; }
    for (;;) { cx++; dl = TP(2, 0)[cx]; if (dl == 0x29) break; TP(2, 1)[ax++] = (char)dl; }
    for (;;) { cx++; if (!(cx < *len)) break; TP(2, 1)[ax++] = TP(2, 0)[cx]; }
    cx = (int16_t)(*left - 1);
    if (cx < ax) {
        for (int16_t i = cx; i < ax; i++) TP(2, 0)[i] = TP(2, 1)[i];
        cx = ax;
    }
    *len = cx;
    return 1;
}

#define T(k) GP(char, *DLLPTR(char, rule_vm[inst].tape[k]))

uint32_t vm_proc(int inst, uint8_t n, int16_t *left, int16_t *len, uint32_t prev_eax) {
    const RuleProc *p = n < 62 ? &rule_procs[inst][n] : NULL;
    uint32_t al1 = (prev_eax & ~0xffu) | 1u;
    switch (p ? p->kind : PROC_GUEST) {
    case PROC_MAIN: {
        // (the original passes *left with the upper half of the pointer to it; only the low half is used)
        uint32_t e = vm_main(inst, p->mode, *left, len);
        return p->res < 0 ? e : (e & ~0xffu) | (uint8_t)p->res;
    }
    case PROC_LEX:
        return (prev_eax & ~0xffu) | lex_lookup(&lex_dicts[p->mode], left, len);
    case PROC_LEXMAIN:
        return (prev_eax & ~0xffu) | lex_main(left, len);
    case PROC_TAGS: return (prev_eax & ~0xffu) | proc_tags(left, len);
    case PROC_PAUSE_VERB: return (prev_eax & ~0xffu) | proc_pause_verb(left, len);
    case PROC_PAUSE_B: return (prev_eax & ~0xffu) | proc_pause_b(left, len);
    case PROC_COPY_3_0_B: return (prev_eax & ~0xffu) | proc_copy_3_0_b(left, len);
    case PROC_CLASS_B: return (prev_eax & ~0xffu) | proc_class_b(left, len);
    case PROC_LOOKUP_LOOP: return (prev_eax & ~0xffu) | proc_lookup_loop(left, len);
    case PROC_PAREN_OR: return (prev_eax & ~0xffu) | proc_paren_or(left, len);
    case PROC_FALSE:
        return prev_eax & ~0xffu;
    case PROC_CHOOSE:                       // A, 13: one of two rule sets, by a global flag
        return (uint8_t)vm_proc(inst, *DLLVAR(int32_t, 0x63738c98) ? 15 : 14, left, len, prev_eax);
    case PROC_FILL_FORWARD: {               // A, 3: tape 2: fill filler with the last real character
        char fill = 0x7e;
        int32_t last = *left;
        for (int32_t i = *left; i <= *len; i++) {
            char c = T(2)[i - 1];
            if (c != 0x7e) { fill = c; last = i; }
            else T(2)[i - 1] = fill;
        }
        for (int32_t j = *left - 1; j <= last - 2; j++) T(2)[j] = fill;
        return al1;
    }
    case PROC_CLEAR4:                       // A, 4: tape 4 of the segment becomes filler
        for (int32_t i = *left - 1; i < *len; i++) T(4)[i] = 0x7e;
        return al1;
    case PROC_QUOTES:                       // A, 12: move tape 3's mark of a quote / G over following filler
        for (int16_t i = *left; i < *len; i++) {
            uint8_t c = (uint8_t)T(0)[i - 1];
            if (c != 0x22 && c != 0x27 && c != 0x60 && c != 0x47) continue;
            char d = T(3)[i - 1];
            if (d == 0x7e) continue;
            int16_t k = 1;
            if (T(0)[i] == 0x7e)
                do k++; while (T(0)[k + i - 1] == 0x7e);
            T(3)[k + i - 1] = d;
        }
        return al1;
    case PROC_NUMBER_DASHES: {              // C, 3: number the dashes of tape 0 from the end on tape 3
        int16_t k = 1;
        for (int16_t i = (int16_t)(*len - 1); i >= *left - 1; i--) {
            if (T(0)[i] != 0x2d) continue;
            if (k < 10) T(3)[i] = (char)(k + 0x30);
            k++;
        }
        return al1;
    }
    case PROC_BLANK3:                       // C, 4
        for (int16_t i = (int16_t)(*left - 1); i < *len; i++) T(3)[i] = 0x20;
        return al1;
    case PROC_COPY_1_0:                     // C, 23
        for (int16_t i = (int16_t)(*left - 1); i < *len; i++) T(0)[i] = T(1)[i];
        return al1;
    case PROC_COPY_3_0:                     // C, 52
        for (int16_t i = (int16_t)(*left - 1); i < *len; i++) T(0)[i] = T(3)[i];
        return al1;
    case PROC_COPY_0_123:                   // C, 31
        for (int16_t i = (int16_t)(*left - 1); i < *len; i++) {
            T(1)[i] = T(0)[i];
            T(2)[i] = T(0)[i];
            T(3)[i] = T(0)[i];
        }
        return al1;
    default:
        return (prev_eax & ~0xffu) | vm_proc_guest(inst, n, left, len);
    }
}

uint8_t vm_callback(int inst, uint8_t n, int16_t *left, int16_t *len) {
    return (uint8_t)vm_proc(inst, n, left, len, 0);
}

int vm_proc_is_c(int inst, uint8_t n) { return n < 62 && rule_procs[inst][n].kind != PROC_GUEST; }
