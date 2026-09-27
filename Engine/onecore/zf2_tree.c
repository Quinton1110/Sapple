/* zf2_tree.c - utterance tree of the zf2 stage: word predicates, syllable/phone construction from the
 * pronunciation, the sentence rebuild with pause insertion (FUN_18001a644) and the phrase / prosodic-word /
 * intonation-phrase structure (FUN_180045c80).  Addresses: MSTTSEngine_OneCore.dll 10.3.21207 (notes/fe2.md). */
#include <stdlib.h>
#include <string.h>
#include "zf2_int.h"

/* ---- phone set (MSTTSLocEnUS.dat resource 29a5584b: {u16 id, name, u32 flags}) ---- */
const char *const z2_phone_name[Z2_MAXPH] = {
    "", "&", "-", "-SIL-", "-SP-", "1", "2", "AA", "AE", "AH", "AO", "AW", "AX", "AY", "B", "CH", "D", "DH", "EH",
    "ER", "EY", "F", "G", "H", "IH", "", "IY", "JH", "K", "L", "M", "N", "NG", "OW", "OY", "P", "R", "S", "SH", "T",
    "TH", "UH", "UW", "V", "W", "Y", "Z", "ZH"};
const uint32_t z2_phone_flags[Z2_MAXPH] = {
    0, 0x40000000, 0x40000000, 0, 0, 0x10000000, 0x20000000, 0xd, 0xd, 0xd, 0xd, 0xd, 0xd, 0xd, 0x82a, 0x8082,
    0x402a, 0x204a, 0xd, 0xd, 0xd, 0x1042, 0x1002a, 0x20042, 0xd, 0, 0xd, 0x808a, 0x10022, 0x420e, 0x90e, 0x410e,
    0x1010e, 0xd, 0xd, 0x822, 0x820e, 0x4042, 0x8042, 0x4022, 0x2042, 0xd, 0xd, 0x104a, 0xc0e, 0x840e, 0x404a, 0x8042};
#define PF_SYLB 0x40000000u
#define PF_STRESS 0x10000000u
#define PF_TONE 0x20000000u
#define PF_SKIP 0x80000000u

int z2_phone_id(const z2_phoneset *ps, const char *name, int len)
{   /* ClassicVoices patch: the voice's phone set */
    int i;
    for (i = 1; i < Z2_PS_MAX; i++)
        if ((int)strlen(ps->name[i]) == len && len && !strncmp(ps->name[i], name, (size_t)len)) return i;
    return -1;
}
static uint32_t pflags(const Z2Sent *s, int id) { return id > 0 && id < Z2_PS_MAX ? s->v->ps.flags[id] : 0; }

/* ---- word predicates ---- */
int z2_is_break(const Z2W *w) { return w->type == 3; }
int z2_is_sapi_silence(const Z2Sent *s, const Z2W *w)
{   /* vt+0x278: break token whose fragment is a SAPI <silence> (action 1) */
    return w->type == 3 && w->frag >= 0 && s->frag[w->frag].action == 1;
}
int z2_pron_len(const Z2W *w) { return w->pron_null ? 0 : w->npron; }
int z2_is_rw(const Z2W *w) { return (w->type & ~4) == 0 && z2_pron_len(w) != 0; }
int z2_has_syl_pron(const Z2W *w)
{   /* FUN_18001a56c */
    if (!((w->type & ~4) == 0 || z2_is_break(w))) return 0;
    return w->ns != 0 && z2_pron_len(w) != 0;
}
int z2_is_empty_word(const Z2W *w) { return (w->type & ~4) == 0 && z2_pron_len(w) == 0; }
int z2_word_skip(const Z2W *w)
{   /* FUN_18001a070 */
    return !(!z2_is_break(w) && w->type != 1 && w->ns != 0 && z2_pron_len(w) != 0);
}
int z2_prev_rw(const Z2Sent *s, int i)
{
    for (i--; i >= 0; i--) if (z2_is_rw(&s->w[i])) return i;
    return -1;
}
int z2_next_sp(const Z2Sent *s, int i)
{
    for (i++; i < s->nw; i++) if (z2_has_syl_pron(&s->w[i])) return i;
    return -1;
}
int z2_prev_sp(const Z2Sent *s, int i)
{
    for (i--; i >= 0; i--) if (z2_has_syl_pron(&s->w[i])) return i;
    return -1;
}
int z2_phone_is_sil(const Z2Sent *s, int id)
{   /* FUN_18001d540: SIL id (+0x1c) or SP id (+0x1e, -1 when [ShortPause] Enable=false) */
    return id == s->v->ps.sil || (s->v->short_pause >= 0 && id == s->v->short_pause);
}
int z2_syl_is_sil(const Z2Sent *s, int si)
{
    const Z2S *y = &s->s[si];
    return y->np == 1 && z2_phone_is_sil(s, s->p[y->p0].id);
}
int z2_phrase_is_break(const Z2Sent *s, int phi)
{
    const Z2PH *p = &s->ph[phi];
    return p->w0 == p->w1 && p->w0 >= 0 && z2_is_break(&s->w[p->w0]);
}
int z2_ip_is_break(const Z2Sent *s, int ipi)
{
    const Z2IP *p = &s->ip[ipi];
    return p->ph0 == p->ph1 && p->ph0 >= 0 && z2_phrase_is_break(s, p->ph0);
}

/* ---- growable arrays ---- */
#define GROW(arr, n, cap)                                                                   \
    do {                                                                                    \
        if ((n) >= (cap)) {                                                                 \
            (cap) = (cap) ? 2 * (cap) : 32;                                                 \
            (arr) = realloc((arr), (size_t)(cap) * sizeof *(arr));                          \
        }                                                                                   \
    } while (0)

int z2_insert_word(Z2Sent *s, int at, const Z2W *w)
{
    int i, q, e;
    GROW(s->w, s->nw, s->capw);
    memmove(&s->w[at + 1], &s->w[at], (size_t)(s->nw - at) * sizeof *s->w);
    s->w[at] = *w;
    s->nw++;
    for (i = 0; i < s->nw; i++)
        if (i != at && s->w[i].attached >= at) s->w[i].attached++;
    for (q = 0; q < s->nquotes; q++) {
        if (s->quotes[q].open >= at) s->quotes[q].open++;
        if (s->quotes[q].close >= at) s->quotes[q].close++;
    }
    for (q = 0; q < s->npar; q++)
        for (e = 0; e < s->par[q].nel; e++) {
            if (s->par[q].el[e].a >= at) s->par[q].el[e].a++;
            if (s->par[q].el[e].b >= at) s->par[q].el[e].b++;
        }
    return at;
}

/* FUN_180019470: ToBIAccent of the word's first syllable with stress 1 := word+0x22c */
static int syl_stress(const Z2Sent *s, int si)
{   /* FUN_180019540 -> FUN_1800195b4: max over phones of (stress mark id - 5 + 1) */
    const Z2S *y = &s->s[si];
    int k, m = 0;
    for (k = 0; k < y->np; k++) {
        int st = s->p[y->p0 + k].st, v = st ? st - s->v->ps.st1 + 1 : 0;
        if (v > m) m = v;
    }
    return m;
}
void z2_set_tobi_accent(Z2Sent *s, int wi)
{
    Z2W *w = &s->w[wi];
    int k;
    for (k = 0; k < w->ns; k++)
        if (syl_stress(s, w->s0 + k) == 1) { s->s[w->s0 + k].tobi = w->i22c; return; }
}

/* FUN_18001aa28: syllables + phones of one word from its pronunciation (split at '&' / '-') */
static void build_word_syls(Z2Sent *s, int wi)
{
    Z2W *w = &s->w[wi];
    int n = z2_pron_len(w), i = 0;
    w->s0 = s->ns;
    w->ns = 0;
    while (i < n) {
        int j = i, k, first = -1, last = -1;
        while (j < n && !(pflags(s, w->pron[j]) & PF_SYLB)) j++;
        if (j > i) {
            GROW(s->s, s->ns, s->caps);
            s->s[s->ns].word = wi;
            s->s[s->ns].p0 = s->np;
            s->s[s->ns].np = 0;
            s->s[s->ns].tobi = 0;
            for (k = i; k < j; k++) { /* FUN_18001c4ac */
                int id = w->pron[k];
                uint32_t f = pflags(s, id);
                /* stress ('1') or tone ('2') mark -> +0x80 of the previous phone; flag 0x80000000 -> +0x82 */
                if (f & (PF_STRESS | PF_TONE)) { if (last >= 0) s->p[last].st = id; }
                else if (f & PF_SKIP) { if (last >= 0) s->p[last].tone = id; }
                else {
                    GROW(s->p, s->np, s->capp);
                    s->p[s->np].syl = s->ns;
                    s->p[s->np].id = id;
                    s->p[s->np].st = 0;
                    s->p[s->np].tone = 0;
                    last = s->np++;
                    if (first < 0) first = last;
                    s->s[s->ns].np++;
                }
            }
            if (s->s[s->ns].np) { s->ns++; w->ns++; }
        }
        i = j + 1;
    }
    if (!w->ns) w->s0 = -1;
    z2_set_tobi_accent(s, wi);
}

/* FUN_1800771bc: pause phone of a break word */
static int pause_phone(const Z2Sent *s, int wi)
{
    int sp = s->v->short_pause;
    if (sp != -1 && s->w[wi].bi != 5 && wi > 0) return sp;
    return s->v->ps.sil;
}

/* FUN_18001fb2c: the single SIL/SP syllable of a break token */
static void build_break_syl(Z2Sent *s, int wi)
{
    Z2W *w = &s->w[wi];
    int id = pause_phone(s, wi);
    w->lang = 0x409;
    free(w->pron);
    w->pron = (uint16_t *)malloc(sizeof(uint16_t));
    w->pron[0] = (uint16_t)id;
    w->npron = 1;
    w->pron_null = 0;
    GROW(s->s, s->ns, s->caps);
    GROW(s->p, s->np, s->capp);
    s->s[s->ns].word = wi;
    s->s[s->ns].p0 = s->np;
    s->s[s->ns].np = 1;
    s->s[s->ns].tobi = 0;
    s->p[s->np].syl = s->ns;
    s->p[s->np].id = id;
    s->p[s->np].st = 0;
    s->p[s->np].tone = 0;
    w->s0 = s->ns;
    w->ns = 1;
    s->ns++;
    s->np++;
}

/* FUN_18001afac: pause class from the break index */
static void normalize_pause(Z2Sent *s, int wi)
{
    Z2W *w = &s->w[wi];
    int j, nextword = 0;
    for (j = wi + 1; j < s->nw; j++)
        if (!z2_is_break(&s->w[j]) && !z2_is_empty_word(&s->w[j])) break;
    if (j < s->nw && (s->w[j].type == 0 || s->w[j].type == 4)) nextword = 1;
    if (w->bi == 3) { if (w->pause < 1) w->pause = 1; }
    else if (w->bi == 4) { if (!(w->pause < 6 && w->pause > 2)) w->pause = nextword ? 3 : 6; }
    else if (w->bi == 5) w->pause = 7;
    else w->pause = 0;
}

/* FUN_1800199a8: silence (ms) after a word */
static int word_silence(const Z2Sent *s, const Z2W *w)
{
    const zf2_voice *v = s->v;
    int ms;
    if (w->i214) return w->i214;
    if ((unsigned)w->pause >= 8) return 0;
    ms = v->sil[w->pause];
    if (v->no_pause && w->pause >= 3 && w->pause <= 6) ms = 0;
    if (w->has_ne && w->ne_obj == 2) ms = (int)(((unsigned)ms * (unsigned)v->ratio_ne) / 100u);
    if (w->i2b8 == 1 && ms != 0) ms = v->speak_end;
    return ms;
}

/* FUN_1800777cc: a new break token modelled on word src */
static Z2W make_pause_word(const Z2Sent *s, int src)
{
    Z2W n;
    const Z2W *w = &s->w[src];
    memset(&n, 0, sizeof n);
    n.type = 3;
    n.frag = w->frag;
    n.lang = w->lang;
    n.src_off = w->src_off;
    n.src_len = 0;
    n.bi = w->bi;
    n.pause = w->pause;
    n.pos = n.tpos = 0xFFFF;
    n.i180 = 8;
    n.attached = -1;
    n.s0 = -1;
    n.phrase = n.pw = -1;
    n.inserted = 1;
    n.pron = NULL;
    n.npron = 0;
    n.pron_null = 0;
    return n;
}

/* FUN_18001a644 (sentence vt+0xa0 -> FUN_18005ff70): rebuild syllables/phones, insert pause tokens */
void z2_rebuild(Z2Sent *s)
{
    const zf2_voice *v = s->v;
    int i;
    s->ns = s->np = 0;
    for (i = 0; i < s->nw; i++) { s->w[i].s0 = -1; s->w[i].ns = 0; }
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        if (!z2_is_break(w) && !w->pron_null && w->lang != 0) {
            int lead = 0;
            if (s->np == 0) lead = 1;
            else {
                int k = s->np - 1;
                while (k >= 0 && z2_is_sapi_silence(s, &s->w[s->s[s->p[k].syl].word])) k--;
                if (k < 0) lead = 1;
            }
            if (lead && v->sil[Z2_PC_SENT] != 0) {
                int len = 100;
                if (w->i2bc == 1) len = v->speak_start;
                if (len != 0) {
                    Z2W n = make_pause_word(s, i);
                    n.i210 = len;
                    n.bi = 5;
                    n.pause = 7;
                    /* FUN_180068c6c moves a bookmark of the word to the new pause token */
                    n.bookmark = w->bookmark;
                    n.bookmark_name = w->bookmark_name;
                    w->bookmark = 0;
                    w->bookmark_name = NULL;
                    z2_insert_word(s, i, &n);
                    build_break_syl(s, i);
                    continue; /* next iteration handles the original word again (now at i+1) */
                }
            }
            build_word_syls(s, i);
            w = &s->w[i];
            normalize_pause(s, i);
            {
                int ms = word_silence(s, w);
                if (ms) {
                    if (w->attached < 0) {
                        int j;
                        for (j = i + 1; j < s->nw && z2_is_empty_word(&s->w[j]); j++);
                        if (j >= s->nw || !z2_is_sapi_silence(s, &s->w[j])) {
                            Z2W n = make_pause_word(s, i);
                            n.i210 = ms;
                            z2_insert_word(s, i + 1, &n);
                            s->w[i].attached = i + 1;
                        }
                    } else
                        s->w[w->attached].i210 = ms;
                }
            }
        } else if (z2_is_break(w))
            build_break_syl(s, i);
    }
    s->rebuild_flag = 0;
}

/* ---- phrase structure: FUN_180045c80 (sentence vt+0xa8) with param_2 = p2 ---- */
static void new_phrase(Z2Sent *s, int a, int b)
{
    int k;
    GROW(s->ph, s->nph, s->capph);
    s->ph[s->nph].w0 = a;
    s->ph[s->nph].w1 = b;
    s->ph[s->nph].ip = -1;
    s->ph[s->nph].pw0 = s->ph[s->nph].pw1 = -1;
    if (a >= 0 && b >= a)
        for (k = a; k <= b; k++) s->w[k].phrase = s->nph;
    s->nph++;
}
static void new_ip(Z2Sent *s, int a, int b)
{
    int k;
    GROW(s->ip, s->nip, s->capip);
    s->ip[s->nip].ph0 = a;
    s->ip[s->nip].ph1 = b;
    if (a >= 0 && b >= a)
        for (k = a; k <= b; k++) s->ph[k].ip = s->nip;
    s->nip++;
}
static void new_pw(Z2Sent *s, int a, int b, int ph)
{
    int k;
    GROW(s->pw, s->npw, s->cappw);
    s->pw[s->npw].w0 = a;
    s->pw[s->npw].w1 = b;
    s->pw[s->npw].ph = ph;
    for (k = a; k <= b && k >= 0; k++) s->w[k].pw = s->npw;
    s->npw++;
}

static void build_pws(Z2Sent *s, int p2)
{   /* FUN_180045e94 */
    int phi;
    s->npw = 0;
    for (phi = 0; phi < s->nph; phi++) {
        int first = s->ph[phi].w0, last = s->ph[phi].w1, a, x;
        int pw0 = -1, pw1 = -1;
        a = first;
        if (p2 && a >= 0 && !z2_has_syl_pron(&s->w[a])) a = z2_next_sp(s, a);
        x = a;
        while (x >= 0 && x < s->nw) {
            if (!p2 || z2_has_syl_pron(&s->w[x])) {
                if (s->w[x].bi > 1 || x == last) {
                    new_pw(s, a, x, phi);
                    if (pw0 < 0) pw0 = s->npw - 1;
                    pw1 = s->npw - 1;
                    a = p2 ? z2_next_sp(s, x) : (x + 1 < s->nw ? x + 1 : -1);
                    if (x == last) break;
                }
                x = x + 1 < s->nw ? x + 1 : -1;
            } else
                x = z2_next_sp(s, x);
        }
        s->ph[phi].pw0 = pw0;
        s->ph[phi].pw1 = pw1;
    }
}

static void build_ips(Z2Sent *s, int p2)
{   /* FUN_1800b58ac */
    int p = s->nph ? 0 : -1, start = p;
    s->nip = 0;
    while (p >= 0) {
        int next = p + 1 < s->nph ? p + 1 : -1, lastw = s->ph[p].w1;
        if (s->w[lastw].bi > 3 || next < 0) {
            if (!z2_phrase_is_break(s, p)) {
                int endp = p;
                if (p2) {
                    int k, has = 0;
                    for (k = s->ph[p].w0; k <= s->ph[p].w1; k++) if (z2_has_syl_pron(&s->w[k])) { has = 1; break; }
                    if (!has) { /* FUN_18005c138(p, 1): previous non-break phrase */
                        int q = p - 1;
                        while (q >= 0 && z2_phrase_is_break(s, q)) q--;
                        endp = q;
                    }
                }
                new_ip(s, start, endp);
            } else {
                if (!z2_phrase_is_break(s, start) && start != p) {
                    int q = p - 1; /* FUN_18005c138(p,1) */
                    while (q >= 0 && z2_phrase_is_break(s, q)) q--;
                    new_ip(s, start, q);
                    start = p;
                }
                for (;;) {
                    if (start < 0) break;
                    new_ip(s, start, start);
                    if (start == p) break;
                    start = start + 1 < s->nph ? start + 1 : -1;
                }
            }
            {
                int nw = z2_next_sp(s, lastw);
                next = nw >= 0 ? s->w[nw].phrase : -1;
                start = next;
            }
        }
        p = next;
    }
    s->first_ip = s->nip ? 0 : -1;
    s->last_ip = s->nip ? s->nip - 1 : -1;
}

void z2_build_phrases(Z2Sent *s, int p2)
{
    int i, a, x;
    for (i = 0; i < s->nw; i++) { s->w[i].phrase = -1; s->w[i].pw = -1; }
    s->nph = 0;
    if (s->nw) {
        x = 0;
        if (p2 && !z2_has_syl_pron(&s->w[0])) x = z2_next_sp(s, 0);
        a = x;
        while (x >= 0) {
            int nx = x + 1 < s->nw ? x + 1 : -1;
            Z2W *w = &s->w[x];
            if (w->bi > 2 || nx < 0 || z2_is_break(w)) {
                if (!z2_is_break(w)) {
                    int e = x;
                    if (p2 && !z2_has_syl_pron(w)) e = z2_prev_sp(s, x);
                    new_phrase(s, a, e);
                } else {
                    if (a != x) {
                        int e = p2 ? z2_prev_sp(s, x) : x - 1;
                        new_phrase(s, a, e);
                    }
                    new_phrase(s, x, x);
                }
                nx = p2 ? z2_next_sp(s, x) : nx;
                a = nx;
            }
            x = nx;
        }
    }
    build_pws(s, p2);
    build_ips(s, p2);
}

/* the tree as CTextProcessor built it (FUN_18001a644 first pass: all pause classes still 0 -> no insertion) */
void z2_initial_build(Z2Sent *s)
{
    int i;
    s->ns = s->np = 0;
    for (i = 0; i < s->nw; i++) {
        Z2W *w = &s->w[i];
        w->s0 = -1;
        w->ns = 0;
        if (z2_is_break(w)) build_break_syl(s, i);
        else if (!w->pron_null && w->lang != 0) build_word_syls(s, i);
    }
}
