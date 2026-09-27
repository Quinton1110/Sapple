/* zf1_pron_oov.c - LTA OOV splitter (loc!18000a1d0 = CLocaleTextAnalyzerENU vt+0x28) and COMPOUNDPRON
 * (CWordPronouncer mode 8, FUN_1800cca70).  Portable C99.  See notes/fe1_pron.md.
 *
 * OOV splitter for en-US (NE categories sp:email / sp:webUrl / sp:filePath use CEmailWordBreaker, not ported:
 * TN expands those entities into words before the pronouncer runs):
 *   CCompoundWordBreakerEnUS (loc!18003e1bc ctor, params +0x34=2 max parts, +0x38=4 min part, +0x3c=16 max known
 *   length, +0x40=4, +0x44=8 min word length):
 *     vt+0x20 loc!180018820: all chars letters, 8 <= len <= 32, no '-' '_', not "eLearning"
 *     vt+0x10 loc!18003e310 / loc!18003f4d8: first split (left length 4 .. N-4) where both halves are known words
 *             (loc!18003ed6c state 2: length <= 16 and found in the container lexicon, loc!18000a06c);
 *             a known whole word is not split.  Splits found by the later loops always contain a part
 *             shorter than 4 and are merged into one part, i.e. rejected
 *     vt+0x08 / vt+0x18: merge parts shorter than 4, accept exactly 2 parts
 *     vt+0x28 loc!1800188b0: each part -> first pronunciation of its lexicon entry (CWordPronouncer mode 1);
 *             reject when it has more than len+3 phones (keeping the parts appended so far); join with '&'
 *   no split and the word is 1..4 upper-case letters A-Z -> spelled (mode 3). */
#include "zf1_pron.h"
#include "zf1_fst.h"
#include <stdlib.h>
#include <string.h>

static int u16len(const uint16_t *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int is_letter(zf_char c) { return zf_tolower(c) != c || zf_toupper(c) != c; }

/* loc!18003ea78 / loc!18000a06c: known = length <= 16 and found in the container lexicon (LEX + CEnMorph) */
static int known(zf1_pron *p, const zf_char *s, int n, zf1_lexhit *h)
{
    if (n <= 0 || n > 16) return 0;
    return zf1_pron_lookup_dom(p, s, n, p->curdom, h);
}

static uint32_t flg(zf1_pron *p, uint16_t id)
{
    uint32_t f = 0;
    zf1_phone_flags(p->ps, id, &f);
    return f;
}

/* CPhoneme vt+0x130: append with '&' separator */
static int append_part(zf1_pron *p, uint16_t *out, int cap, const uint16_t *src)
{
    int n = u16len(out), m = u16len(src), i;
    if (!m) return 0;
    if (n) {
        if (n + 1 >= cap) return -1;
        out[n++] = p->sep;
    }
    if (n + m + 1 > cap) return -1;
    for (i = 0; i < m; i++) out[n + i] = src[i];
    out[n + m] = 0;
    return 0;
}

/* ---- loc!18003e310 split: entries e[i] = (left word[0..N-1-i), right word[N-1-i..N)) ---- */
typedef struct cseg { int off, len; } cseg;
typedef struct centry { cseg l, r; } centry;
typedef struct cpart { int off, len; } cpart;

static int seg_valid(zf1_pron *p, const zf_char *t, cseg s, zf1_lexhit *h)   /* loc!18003ed6c */
{
    if (s.len == 0) return 1;
    return known(p, t + s.off, s.len, h);
}

/* loc!18003f4d8 */
static int split_both(zf1_pron *p, const zf_char *t, centry *e, int *pa, int *pb, zf1_lexhit *h)
{
    int a = *pa, b = *pb, k, lo;
    for (k = b - 4; k >= a + 3; k--) {
        if (k == b || k == a - 1) continue;
        if (seg_valid(p, t, e[k].l, h) && seg_valid(p, t, e[k].r, h)) goto found;
    }
    for (k = b - 1; !(k <= b - 4 || k < a); k--)
        if (seg_valid(p, t, e[k].l, h) && seg_valid(p, t, e[k].r, h)) goto found;
    lo = a + 2;
    if (b - 5 < lo) lo = b - 5;
    for (k = lo; k >= a; k--)
        if (seg_valid(p, t, e[k].l, h) && seg_valid(p, t, e[k].r, h)) goto found;
    return 0;
found:
    *pa = *pb = k;
    return 1;
}

/* loc!18003f384: longest known right part */
static int split_suffix(zf1_pron *p, const zf_char *t, centry *e, int *pa, int b, cpart *out, zf1_lexhit *h)
{
    int k, m;
    for (k = b - 1; k >= *pa; k--) {
        if (e[k].r.len != 0 && seg_valid(p, t, e[k].r, h)) {
            int len = e[k].r.len;
            out->off = e[k].r.off;
            out->len = len;
            for (m = k + 1; m <= b; m++) e[m].r.len -= len;
            *pa = k + 1;
            return 1;
        }
    }
    return 0;
}

/* loc!18003f278: longest known left part */
static int split_prefix(zf1_pron *p, const zf_char *t, centry *e, int a, int *pb, cpart *out, zf1_lexhit *h)
{
    int k, m;
    for (k = a; k <= *pb - 1; k++) {
        if (seg_valid(p, t, e[k].l, h)) {
            int len = e[k].l.len;
            out->off = e[k].l.off;
            out->len = len;
            for (m = a; m <= k; m++) { e[m].l.len -= len; e[m].l.off += len; }
            *pb = k;
            return 1;
        }
    }
    return 0;
}

static int all_upper(const zf_char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (zf_tolower(s[i]) == s[i]) return 0;
    return 1;
}

static int compound_breaker(zf1_pron *p, const zf_char *t, uint16_t *out, int cap)
{
    int n = (int)zf_strlen(t), i;
    static const char el[] = "elearning";
    zf1_lexhit *h;
    centry *e;
    cpart parts[80], sufs[80];
    int np = 0, ns = 0, a, b, count = 0, done = 0, status = 0, u19 = 0;
    for (i = 0; i < n; i++)
        if (!is_letter(t[i])) return 1;
    if (n < 8 || n > 32) return 1;
    if (n == 9) {
        for (i = 0; i < 9 && zf_tolower(t[i]) == (zf_char)el[i]; i++) {}
        if (i == 9) return 1;
    }
    h = (zf1_lexhit *)malloc(sizeof *h);
    e = (centry *)malloc(sizeof(centry) * (size_t)n);
    if (!h || !e) { free(h); free(e); return -1; }
    for (i = 0; i < n; i++) {
        e[i].l.off = 0; e[i].l.len = n - 1 - i;
        e[i].r.off = n - 1 - i; e[i].r.len = i + 1;
    }
    a = 0;
    b = n - 1;
    for (;;) {
        do {
            if (done) goto finish;
            if (e[b].l.len == 0 && seg_valid(p, t, e[b].r, h)) {
                parts[np].off = e[b].r.off; parts[np++].len = e[b].r.len;
                done = 1;
                continue;
            }
            if (split_both(p, t, e, &a, &b, h)) {
                int k = a;
                parts[np].off = e[k].l.off; parts[np++].len = e[k].l.len;
                count += (e[k].l.len >= 4);
                parts[np].off = e[k].r.off; parts[np++].len = e[k].r.len;
                count += (e[k].r.len >= 4);
                done = 1;
                continue;
            } else {
                cpart c;
                if (split_suffix(p, t, e, &a, b, &c, h)) {
                    memmove(sufs + 1, sufs, sizeof(cpart) * (size_t)ns);
                    sufs[0] = c;
                    ns++;
                    u19 = a;
                    count += (c.len >= 4);
                } else {
                    u19 = a;
                    if (split_prefix(p, t, e, a, &b, &c, h)) {
                        parts[np++] = c;
                        count += (c.len >= 4);
                    } else {
                        c.off = e[a].l.off;
                        c.len = e[a].r.len + e[a].l.len;
                        memmove(sufs + 1, sufs, sizeof(cpart) * (size_t)ns);
                        sufs[0] = c;
                        ns++;
                        done = 1;
                        continue;
                    }
                }
            }
        } while (count <= 2 && np < 60 && ns < 60);
        if (!done) {
            parts[np].off = e[u19].l.off;
            parts[np++].len = e[u19].r.len + e[u19].l.len;
            status = 1;
        }
        done = 1;
    }
finish:
    for (i = 0; i < ns; i++) parts[np++] = sufs[i];
    free(e);
    if (status) { free(h); return 1; }
    /* vt+0x08 (loc!18003f140): merge a part shorter than 4 with the following one */
    for (i = 0; i + 1 < np;) {
        if (parts[i].len >= 4 && parts[i + 1].len >= 4) { i++; continue; }
        parts[i].len += parts[i + 1].len;
        memmove(parts + i + 1, parts + i + 2, sizeof(cpart) * (size_t)(np - i - 2));
        np--;
    }
    /* vt+0x18 (loc!18003ec80): exactly 2 parts; each >= 2 chars and (>= 4 or all upper case) */
    if (np == 1 || np > 2) { free(h); return 1; }
    for (i = 0; i < np; i++) {
        if (parts[i].len < 2) { free(h); return 1; }
        if (parts[i].len < 4 && !all_upper(t + parts[i].off, parts[i].len)) { free(h); return 1; }
    }
    /* vt+0x28 (loc!1800188b0): compose */
    out[0] = 0;
    for (i = 0; i < np; i++) {
        const zf_char *s = t + parts[i].off;
        int sl = parts[i].len, k, real = 0;
        const uint16_t *ph;
        if (!zf1_pron_lookup_dom(p, s, sl, p->curdom, h) || !h->p[0].ph[0]) { free(h); return 1; }
        ph = h->p[0].ph;
        for (k = 0; ph[k]; k++)
            if (!(flg(p, ph[k]) & (0x40000000u | 0x10000000u | 0x20000000u))) real++;
        if (real > sl + 3) { free(h); return 1; }   /* engine: count > (len - 1) + 4 */
        if (append_part(p, out, cap, ph) < 0) { free(h); return 0; }
    }
    free(h);
    return 0;
}

/* ---- CEmailWordBreaker (loc!18003df7c): split into known words, concatenate their first prons ---- */
static int eknown(zf1_pron *p, const zf_char *s, int n, zf1_lexhit *h)
{
    return n > 0 && zf1_pron_lookup(p, s, n, h);
}

static int email_breaker(zf1_pron *p, const zf_char *t, uint16_t *out, int cap)
{
    int n = (int)zf_strlen(t), len, k, i, np = 0;
    cpart parts[512];
    zf1_lexhit *h;
    out[0] = 0;
    if (n <= 0 || n > 500) return 0;
    h = (zf1_lexhit *)malloc(sizeof *h);
    if (!h) return -1;
    len = n;                                  /* working string = t[0..len) */
    for (k = len; k > 0 && !eknown(p, t, k, h); k--) {}      /* loc!18003e0cc: longest known prefix */
    while (len > 0 && np < 500) {
        int off = 0;
        while (off < len && !eknown(p, t + off, len - off, h)) off++;   /* loc!18003dcd0 */
        if (off == len) {                    /* nothing known: last char */
            parts[np].off = len - 1; parts[np++].len = 1;
            len--;
        } else if (off < k && k < len) {
            int rem = len - off;
            if (rem <= k) {
                /* tail after the known prefix, split right to left */
                int tl = len - k;
                while (tl > 0 && np < 500) {
                    int o2 = 0;
                    while (o2 < tl && !eknown(p, t + k + o2, tl - o2, h)) o2++;
                    if (o2 == tl) { parts[np].off = k + tl - 1; parts[np++].len = 1; tl--; }
                    else { parts[np].off = k + o2; parts[np++].len = tl - o2; tl = o2; }
                }
                len = k;
            } else {
                parts[np].off = off; parts[np++].len = rem;
                len = off;
            }
        } else {
            parts[np].off = off; parts[np++].len = len - off;
            len = off;
        }
        if (len < k) for (k = len; k > 0 && !eknown(p, t, k, h); k--) {}
    }
    /* parts were collected right to left; the engine's list inserts at the head -> left to right */
    for (i = np - 1; i >= 0; i--) {
        int m, o;
        if (!zf1_pron_lookup(p, t + parts[i].off, parts[i].len, h) || !h->p[0].ph[0]) continue;
        m = u16len(h->p[0].ph);
        o = u16len(out);
        if (o + m + 1 > cap) { free(h); return cap < 0x180 ? -1 : 0; }
        memcpy(out + o, h->p[0].ph, sizeof(uint16_t) * (size_t)(m + 1));
    }
    free(h);
    return 0;
}

int zf1_pron_oov_split(zf1_pron *p, const zf_word *w, uint16_t *out, int cap)
{
    const zf_char *t = w->text;
    int r, n, i;
    if (!t || !*t) return 1;
    out[0] = 0;
    if (w->ne_type && (!zf_strcmp_a(w->ne_type, "sp:email") || !zf_strcmp_a(w->ne_type, "sp:webUrl") ||
                       !zf_strcmp_a(w->ne_type, "sp:filePath"))) {
        r = email_breaker(p, t, out, cap);
        if (r != 1) return r;
    }
    if (w->ne_type && !zf_strcmp_a(w->ne_type, "sp:mixedAcronymWord")) {
        /* CWordPronouncer mode 5 (letters and digits spelled) */
        zf1_pron_spell(p, t, out, cap, 1);
        if (out[0]) return 0;
    }
    r = compound_breaker(p, t, out, cap);
    if (r != 1) return r;
    n = (int)zf_strlen(t);
    if (n < 5) {
        for (i = 0; i < n && t[i] >= 'A' && t[i] <= 'Z'; i++) {}
        if (i == n) {
            zf1_pron_spell(p, t, out, cap, 0);
            return 0;
        }
    }
    return 1;
}

/* ---- COMPOUNDPRON (FUN_1800cca70 -> FUN_1800cc510): parse the word with the 19a6569a network (fork B's
 * runtime, zf1_fst.c), split its output text into tokens; "tok/SPELL" or a single letter -> spelled (mode 3),
 * other tokens -> mode 7 (first lexicon pronunciation, else 1..4 upper-case letters spelled, else LTS mode 4);
 * pronunciations joined with '-' (empty ones skipped).  A single token = not a compound (returns 1). */
static int part_pron(zf1_pron *p, const zf_char *s, int n, int spell, uint16_t *out, int cap)
{
    zf_char tmp[256];
    zf1_lexhit *h;
    int src, i;
    if (n <= 0 || n > 250) return 1;
    memcpy(tmp, s, sizeof(zf_char) * (size_t)n);
    tmp[n] = 0;
    out[0] = 0;
    if (spell) return zf1_pron_spell(p, tmp, out, cap, 0);
    h = (zf1_lexhit *)malloc(sizeof *h);
    if (!h) return -1;
    if (zf1_pron_lookup_dom(p, tmp, n, p->curdom, h) && h->p[0].ph[0]) {        /* mode 1 */
        int k = u16len(h->p[0].ph);
        if (k > cap - 1) k = cap - 1;
        memcpy(out, h->p[0].ph, sizeof(uint16_t) * (size_t)k);
        out[k] = 0;
        free(h);
        return 0;
    }
    free(h);
    if (n < 5) {                                                   /* mode 6: loc!180017f90 */
        for (i = 0; i < n && tmp[i] >= 'A' && tmp[i] <= 'Z'; i++) {}
        if (i == n) return zf1_pron_spell(p, tmp, out, cap, 0);
    }
    /* mode 4; the vowel check scans all cap entries of out, i.e. also stale phones of earlier (longer) pieces
     * of the word: "kk0kveImo" keeps "kve" = K V, "cve" alone is spelled */
    return zf1_pron_lts_word2(p, tmp, out, cap, &src, cap);
}

/* append one output piece of the compound network (FUN_1800ccb60 semantics as implemented by fork A) */
static void cpn_piece(zf1_pron *p, const zf_char *b, int len, uint16_t *part, uint16_t *out, int cap)
{
    static const zf_char spl[] = {'/', 'S', 'P', 'E', 'L', 'L'};
    int spell = 0;
    if (len <= 0) return;
    if (len > 6 && !memcmp(b + len - 6, spl, sizeof spl)) { len -= 6; spell = 1; }
    if (len > 16 && b[0] == '[' && b[len - 15] == ']' && b[len - 14] == '/') {
        /* "[ph+ph]/PRONUNCIATION": explicit phone names */
        zf_char nm[64];
        int i2, j = 0;
        for (i2 = 1; i2 < len - 15 && j < 63; i2++) nm[j++] = (b[i2] == '+') ? ' ' : b[i2];
        nm[j] = 0;
        zf1_phonestr_to_ids(p->ps, nm, part, 0x180);
    } else {
        if (len == 1 && (zf1_iswupper(b[0]) || zf1_iswlower(b[0]))) spell = 1;   /* FUN_18005898c */
        if (part_pron(p, b, len, spell, part, 0x180) < 0) return;
    }
    if (part[0]) {
        int k = u16len(out), m = u16len(part), i2;
        if (k) { if (k + 1 >= cap) return; out[k++] = p->sylmark; }
        if (k + m + 1 > cap) return;
        for (i2 = 0; i2 < m; i2++) out[k + i2] = part[i2];
        out[k + m] = 0;
    }
}

/* ---- COMPOUNDPRON (FUN_1800cca70 -> FUN_1800cc510): the word is walked from position 0: the compound network
 * (fork B's runtime, zf1_fst.c; mode 0, no category, tokenizer 0, no lexicon) is parsed from the position; a
 * parse -> its output text split on ' ', each piece pronounced (FUN_1800ccb60: "tok/SPELL" or a single letter
 * -> spelled (mode 3), "[..]/PRONUNCIATION" explicit phones, else mode 7), position += parsed length; no parse ->
 * the lattice token at the position (FUN_180138c84) is pronounced (single letter: mode 3, else mode 7), unless
 * it is the whole word at position 0 (-> not a compound, returns 1).  Pronunciations joined with '-'. */
int zf1_pron_compound(zf1_pron *p, const zf_char *text, uint16_t *out, int cap)
{
    int n = (int)zf_strlen(text), pos = 0;
    uint16_t part[0x200];   /* local_358: one piece buffer per word, zeroed once, never cleared between pieces */
    if (!p->compound || n <= 0) return 1;
    out[0] = 0;
    memset(part, 0, sizeof part);
    while (pos < n) {
        zf1_netres res;
        memset(&res, 0, sizeof res);
        if (zf1_net_parse((zf1_net *)p->compound, text, n, pos, 0, NULL, 0, &res) < 0) { zf1_netres_free(&res); break; }
        if (res.found && res.len > 0) {
            zf_char *txt = zf1_netres_text(&res), *q, *b;
            int plen = res.len;
            zf1_netres_free(&res);
            for (q = txt; txt && *q;) {
                while (*q == ' ') q++;
                if (!*q) break;
                b = q;
                while (*q && *q != ' ') q++;
                cpn_piece(p, b, (int)(q - b), part, out, cap);
            }
            free(txt);
            pos += plen;
        } else {
            int nxt = zf1_lat_next(0, text, n, pos), len;
            zf1_netres_free(&res);
            if (pos == 0 && nxt >= n) return 1;
            if (nxt > n) nxt = n;
            len = nxt - pos;
            if (part_pron(p, text + pos, len,
                          len == 1 && (zf1_iswupper(text[pos]) || zf1_iswlower(text[pos])),
                          part, 0x180) >= 0 && part[0]) {
                int k = u16len(out), m = u16len(part), i2;
                if (k && k + 1 < cap) out[k++] = p->sylmark;
                if (k + m + 1 <= cap) {
                    for (i2 = 0; i2 < m; i2++) out[k + i2] = part[i2];
                    out[k + m] = 0;
                }
            }
            pos = nxt;
        }
    }
    return 0;
}
