/* zf1_pron.c - CPronouncer port.  See zf1_pron.h and notes/fe1_pron.md.  Portable C99. */
#include "zf1_pron.h"
#include "zf1_morph.h"
#include "zf1_fst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PH_MAX 0x182

/* ---------------- small helpers ---------------- */
static int u16len(const uint16_t *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int u16cmp(const uint16_t *a, const uint16_t *b)
{
    static const uint16_t e = 0;
    if (!a) a = &e;
    if (!b) b = &e;
    for (;; a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}

static int streq_a(const zf_char *s, const char *a) { return zf_strcmp_a(s, a) == 0; }

static uint32_t phflags(const zf1_pron *p, uint16_t id)
{
    uint32_t f = 0;
    if (!zf1_phone_flags(p->ps, id, &f)) return 0;
    return f;
}
#define IS_VOWEL(p, id)  ((phflags(p, id) & 1) != 0)
#define IS_CONS(p, id)   ((phflags(p, id) & 2) != 0)
#define IS_SON(p, id)    ((phflags(p, id) & 4) != 0)
#define IS_MARK(p, id)   ((phflags(p, id) & 0x40000000u) != 0)
#define IS_STR1(p, id)   ((phflags(p, id) & 0x10000000u) != 0)
#define IS_STR2(p, id)   ((phflags(p, id) & 0x20000000u) != 0)

/* ---------------- word pronunciation list (CTTSPronunciationList) ---------------- */
void zf1_word_setpron(zf_word *w, const uint16_t *ph, int src, int force)
{
    int i, pos;
    static const uint16_t empty = 0;
    if (w->i278 == 20 && !force) return;
    if (!ph) ph = &empty;
    /* sorted insert (FUN_18004d8d0): walk while new > existing */
    for (pos = 0; pos < w->nprons; pos++) {
        int c = u16cmp(ph, w->prons[pos]);
        if (c == 0) { w->cur_pron = pos; goto done; }
        if (c < 0) break;
    }
    w->prons = (zf_char **)realloc(w->prons, sizeof(zf_char *) * (size_t)(w->nprons + 1));
    memmove(w->prons + pos + 1, w->prons + pos, sizeof(zf_char *) * (size_t)(w->nprons - pos));
    w->prons[pos] = zf_strndup(ph, (size_t)u16len(ph));
    w->nprons++;
    w->cur_pron = pos;
done:
    if (!ph[0]) { free(w->pron); w->pron = NULL; }
    if (src != 0x17) w->i278 = src;
    (void)i;
}

/* list vt+0x18 only (FUN_180059ea8 path: no i278 change here) */
static void list_add(zf_word *w, const uint16_t *ph)
{
    int saved = w->i278;
    zf1_word_setpron(w, ph, 0x17, 1);
    w->i278 = saved;
}

const uint16_t *zf1_word_curpron(const zf_word *w)
{
    if (w->cur_pron < 0 || w->cur_pron >= w->nprons) return NULL;
    if (!w->prons[w->cur_pron] || !w->prons[w->cur_pron][0]) return NULL;   /* FUN_180041a40: "" -> NULL */
    return w->prons[w->cur_pron];
}

/* ---------------- init ---------------- */
int zf1_pron_init(zf1_pron *p, const zf1_dat *d, const zf1_lex *lx, const zf1_phoneset *ps)
{
    size_t n;
    const uint8_t *r;
    memset(p, 0, sizeof *p);
    p->lx = lx;
    p->ps = ps;
    r = zf1_dat_get(d, 0xac4aefcf, 0xd18325ec, &n);
    if (r && zf1_cart_init(&p->cart, r, n) == 0) p->have_cart = 1;
    r = zf1_dat_get(d, 0xf6e4f50a, 0x80b9a5a3, &n);
    if (r && n >= 4) {
        p->ct.n = zf_rd32(r);
        p->ct.rec = r + 4;
        p->ct.pool = r + 4 + 16 * (size_t)p->ct.n;
    }
    r = zf1_dat_get(d, 0x78f6770d, 0x7ab40d39, &n);
    if (r && n >= 4) {
        p->nonset = zf_rd32(r);
        p->onset = r + 4;
    }
    p->sylmark = (uint16_t)zf1_phone_id(ps, "-", 1);
    p->sep = (uint16_t)zf1_phone_id(ps, "&", 1);
    p->morph = zf1_morph_create(d, lx, ps);
    zf1_pron_load_domains(p, "C:\\Windows\\Speech_OneCore\\Engines\\TTS\\en-US\\");
    r = zf1_dat_get(d, 0x19a6569a, 0xea8d0701, &n);            /* COMPOUNDPRON network */
    if (r) {
        p->compound = zf1_net_load(r, n);
        if (p->compound) {
            zf1_net_set_mask((zf1_net *)p->compound, 1);
            if (getenv("ZF1_CPN_LEX")) zf1_net_set_lexicon((zf1_net *)p->compound, zf1_pron_netlex_cb, p);
        }
    }
    return p->have_cart ? 0 : -1;
}

int zf1_pron_load_domains(zf1_pron *p, const char *dir) { return zf1_pron_load_domains_lcid(p, dir, 1033); }

int zf1_pron_load_domains_lcid(zf1_pron *p, const char *dir, int lcid)
{
    static const char *const names[6] = {"address", "name", "message", "computer", "media", "companyName"};
    static const char *const files[6] = {"Address", "Name", "Message", "Computer", "Media", "CompanyName"};
    const zf1_domfile *loc;
    int i, nloc = zf1_domain_files(lcid, &loc);
    for (i = 0; i < nloc && p->ndom < 8; i++) {   /* ClassicVoices patch: a non-en-US locale's own list */
        char path[1024];
        size_t n;
        const uint8_t *r;
        int k = p->ndom;
        if (strlen(dir) > 900) return -1;
        snprintf(path, sizeof path, "%s%s", dir, loc[i].file);
        if (zf1_dat_load_ci(&p->dom[k].dat, path)) continue;
        r = zf1_dat_get(&p->dom[k].dat, 0x7bd71f46, 0xf9a99c02, &n);
        if (!r || zf1_lex_init(&p->dom[k].lex, r, n)) { zf1_dat_free(&p->dom[k].dat); continue; }
        snprintf(p->dom[k].name, sizeof p->dom[k].name, "%s", loc[i].domain);
        p->ndom++;
    }
    if (nloc) return p->ndom;
    for (i = 0; i < 6 && p->ndom < 8; i++) {
        char path[1024];
        size_t n;
        const uint8_t *r;
        int k = p->ndom;
        if (strlen(dir) > 900) return -1;
        sprintf(path, "%senUS.%s.dat", dir, files[i]);
        if (zf1_dat_load(&p->dom[k].dat, path)) continue;
        r = zf1_dat_get(&p->dom[k].dat, 0x7bd71f46, 0xf9a99c02, &n);
        if (!r || zf1_lex_init(&p->dom[k].lex, r, n)) { zf1_dat_free(&p->dom[k].dat); continue; }
        strcpy(p->dom[k].name, names[i]);
        p->ndom++;
    }
    return p->ndom;
}

void zf1_pron_free(zf1_pron *p)
{
    int i;
    for (i = 0; i < p->ndom; i++) { zf1_lex_free(&p->dom[i].lex); zf1_dat_free(&p->dom[i].dat); }
    zf1_morph_destroy(p->morph);
    if (p->compound) zf1_net_free((zf1_net *)p->compound);
    memset(p, 0, sizeof *p);
}

/* ---------------- container lexicon ---------------- */
static int lex_main(zf1_pron *p, const zf_char *w, int n, zf1_lexhit *out)
{
    zf1_lexent *e = (zf1_lexent *)malloc(sizeof *e);
    int i, k, found;
    if (!e) return 0;
    found = zf1_lex_lookup(p->lx, w, n, e);
    out->n = 0;
    if (found) {
        for (i = 0; i < e->n; i++) {
            zf1_lhpron *hp = &out->p[out->n++];
            memset(hp, 0, sizeof *hp);
            hp->nph = e->p[i].nph;
            memcpy(hp->ph, e->p[i].ph, sizeof(uint16_t) * (size_t)hp->nph);
            hp->ph[hp->nph] = 0;
            hp->source = 0x1000;
            for (k = 0; k < e->p[i].nattr && hp->nset < ZF1_LH_MAXSET; k++) {
                const uint8_t *a = zf1_lex_attrset(p->lx, e->p[i].attrset[k]);
                int m = 0;
                for (; a && zf_rd16(a) && m < ZF1_LH_SETLEN - 1; a += 2) hp->set[hp->nset][m++] = zf_rd16(a);
                hp->set[hp->nset][m] = 0;
                hp->nset++;
            }
        }
    }
    free(e);
    return found && out->n > 0;
}

int zf1_pron_lookup(zf1_pron *p, const zf_char *w, int n, zf1_lexhit *out)
{
    out->n = 0;
    if (!w || n <= 0) return 0;
    if (lex_main(p, w, n, out)) return 1;
    if (p->morph && zf1_morph_lookup(p->morph, w, n, out)) return 1;
    out->n = 0;
    return 0;
}

int zf1_pron_lookup_dom(zf1_pron *p, const zf_char *w, int n, const zf_char *domain, zf1_lexhit *out)
{
    int i, k, j;
    if (domain && *domain && zf_strcmp_a(domain, "none") && zf_strcmp_a(domain, "general")) {
        for (i = 0; i < p->ndom; i++) {
            if (zf_strcmp_a(domain, p->dom[i].name)) continue;
            {
                zf1_lexent *e = (zf1_lexent *)malloc(sizeof *e);
                int found;
                if (!e) break;
                found = zf1_lex_lookup(&p->dom[i].lex, w, n, e);
                out->n = 0;
                if (found) {
                    for (k = 0; k < e->n; k++) {
                        zf1_lhpron *hp = &out->p[out->n++];
                        memset(hp, 0, sizeof *hp);
                        hp->nph = e->p[k].nph;
                        memcpy(hp->ph, e->p[k].ph, sizeof(uint16_t) * (size_t)hp->nph);
                        hp->source = 0x10000;
                        for (j = 0; j < e->p[k].nattr && hp->nset < ZF1_LH_MAXSET; j++) {
                            const uint8_t *a = zf1_lex_attrset(&p->dom[i].lex, e->p[k].attrset[j]);
                            int m = 0;
                            for (; a && zf_rd16(a) && m < ZF1_LH_SETLEN - 1; a += 2) hp->set[hp->nset][m++] = zf_rd16(a);
                            hp->set[hp->nset][m] = 0;
                            hp->nset++;
                        }
                    }
                }
                free(e);
                if (found && out->n) return 1;
            }
        }
    }
    return zf1_pron_lookup(p, w, n, out);
}

int zf1_pron_lexlookup_cb(void *ctx, const zf_char *w, int n, uint16_t *sets, int cap)
{
    zf1_pron *p = (zf1_pron *)ctx;
    zf1_lexhit *h = (zf1_lexhit *)malloc(sizeof *h);
    int k = 0, i, j, m;
    if (!h) return -1;
    if (!zf1_pron_lookup(p, w, n, h)) { free(h); return -1; }
    for (i = 0; i < h->n; i++)
        for (j = 0; j < h->p[i].nset; j++) {
            for (m = 0; h->p[i].set[j][m] && k < cap - 1; m++) sets[k++] = h->p[i].set[j][m];
            if (k < cap) sets[k++] = 0;
        }
    free(h);
    return k;
}

static int map_source(uint32_t s)       /* FUN_1800c349c */
{
    if (s == 1 || s == 2 || s == 0x20) return 5;
    if (s == 0x1000) return 1;
    if (s == 0x4000) return 2;
    if (s == 0x8000) return 4;
    if (s == 0x10000) return 3;
    return 0;
}

static int action_ok(const zf_word *w)
{
    return w->fs.action >= 0 && w->fs.action < 10 && ((0x211u >> w->fs.action) & 1);
}

/* ---------------- pass 1: FUN_180047abc ---------------- */
int zf1_pron_pass1(zf1_pron *p, zf_wordlist *wl)
{
    int i, k, j;
    zf1_lexhit *h = (zf1_lexhit *)malloc(sizeof *h);
    if (!h) return -1;
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        int src, pick;
        if (w->type != ZF_WT_WORD || zf1_word_curpron(w)) continue;
        if (!action_ok(w) || w->lang != 0x409) continue;
        if (!w->text || !zf1_pron_lookup_dom(p, w->text, (int)zf_strlen(w->text), w->s1b0, h)) continue;
        if (w->i84 != 0) continue;
        src = map_source(h->p[0].source);
        for (k = h->n - 1; k >= 0; k--) list_add(w, h->p[k].ph);
        if (src != 0x17) w->i278 = src;
        if (w->fs.pos != 0xFFFF) {
            if (p->setpos) p->setpos(p->posobj, w, w->fs.pos);
            else w->pos = w->tpos = w->fs.pos;
        }
        pick = -1;
        for (k = 0; k < h->n && pick < 0; k++)
            for (j = 0; j < h->p[k].nset && pick < 0; j++) {
                int m;
                for (m = 0; h->p[k].set[j][m]; m++)
                    if (h->p[k].set[j][m] == w->pos) { pick = k; break; }
            }
        if (pick >= 0 && h->p[pick].ph[0]) {
            const uint16_t *cur = zf1_word_curpron(w);
            if (!cur || u16cmp(cur, h->p[pick].ph) != 0) zf1_word_setpron(w, h->p[pick].ph, src, 0);
        }
    }
    free(h);
    return 0;
}

/* ---------------- char table / spelling ---------------- */
static const uint8_t *ct_find(const zf1_pron *p, uint32_t cp)
{
    uint32_t lo = 0, hi = p->ct.n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, v = zf_rd32(p->ct.rec + 16 * (size_t)mid);
        if (v == cp) return p->ct.rec + 16 * (size_t)mid;
        if (v < cp) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

int zf1_pron_charclass(zf1_pron *p, zf_char c)
{
    const uint8_t *r = ct_find(p, c);
    uint16_t f;
    if (!r) return 'O';
    f = zf_rd16(r + 4);
    if (f & 4) return 'C';
    if (f & 1) return 'V';
    return 'O';
}

/* CPhoneme vt+0x130 (FUN_18002a350): append src with separator sep */
static int ph_append(zf1_pron *p, uint16_t *out, int cap, const uint16_t *src)
{
    int n = u16len(out), m = u16len(src), i;
    if (m == 0) return 0;
    if (n != 0) {
        if (cap <= n + 1) goto trunc;
        out[n++] = p->sep;
        out[n] = 0;
    }
    if (n + m + 1 > cap) {
        /* wcscat_s overflow: buffer emptied (s[0]=0) and the engine then cuts back at a separator:
           with the destination reset the loop below leaves "" (engine STRUNCATE behaviour, not observed) */
        goto trunc;
    }
    for (i = 0; i < m; i++) out[n + i] = src[i];
    out[n + m] = 0;
    return 0;
trunc:
    {
        int c = cap;
        do {
            c--;
            if (c <= n || out[c] == p->sylmark) break;
        } while (out[c] != p->sep);
        if (c >= 0 && c < cap) out[c] = 0;
    }
    return 0;
}

static const uint16_t *ct_name2(const zf1_pron *p, const uint8_t *r, uint16_t *tmp, int cap)
{
    const uint8_t *s = p->ct.pool + zf_rd32(r + 12);
    int n = 0;
    while (n < cap - 1 && zf_rd16(s + 2 * n)) { tmp[n] = zf_rd16(s + 2 * n); n++; }
    tmp[n] = 0;
    return tmp;
}

static int is_upper_c(zf_char c) { return zf_tolower(c) != c; }
static int is_lower_c(zf_char c) { return zf_toupper(c) != c; }
static int is_digit_c(zf_char c) { return c >= '0' && c <= '9'; }

int zf1_pron_spell(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int mode5)
{
    uint16_t tmp[64];
    out[0] = 0;
    if (!p->ct.n) return 1;
    for (; *w; w++) {
        const uint8_t *r;
        if (mode5 && !(is_upper_c(*w) || is_lower_c(*w) || is_digit_c(*w))) continue;
        r = ct_find(p, *w);
        if (!r) continue;
        ph_append(p, out, cap, ct_name2(p, r, tmp, 64));
    }
    return 0;
}

/* FUN_18006043c: 1 = spell the word letter by letter */
static int should_spell(zf1_pron *p, const zf_char *w)
{
    int n = (int)zf_strlen(w), i, allup = 1, hv = 0, hc = 0;
    if (!p->ct.n) return 0;
    for (i = 0; i < n; i++)
        if (is_lower_c(w[i]) || !is_upper_c(w[i])) { allup = 0; break; }
    if (allup && n < 5) {
        static const char *const pat[16] = {"VVVV", "CVVV", "VCVV", "CCVV", "VVCV", "CCCV", "VVVC", "VVCC", "VCCC",
                                            "CCCC", "CVCV", "VCVC", "CCVC", "VCCV", "CVVC", "CVCC"};
        static const int val[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
        char cls[5];
        int k;
        if (n < 4) return 1;
        for (i = 0; i < 4; i++) cls[i] = (char)zf1_pron_charclass(p, w[i]);
        cls[4] = 0;
        for (k = 0; k < 16; k++)
            if (!strcmp(cls, pat[k])) return val[k];
        return 0;
    }
    for (i = 0; i < n; i++) {
        int c = zf1_pron_charclass(p, w[i]);
        if (!hv) hv = (c == 'V');
        if (!hc) hc = (c == 'C');
    }
    if (hv && hc) return 0;
    return 1;
}

int zf1_pron_lts_word(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int *src)
{
    return zf1_pron_lts_word2(p, w, out, cap, src, -1);
}

/* vscan: entries of out the vowel check (CPhoneme vt+0x138) scans; -1 = the result only.  0x1800c924c passes the
 * buffer capacity, so stale phones after the terminator count (COMPOUNDPRON's reused piece buffer). */
int zf1_pron_lts_word2(zf1_pron *p, const zf_char *w, uint16_t *out, int cap, int *src, int vscan)
{
    out[0] = 0;
    if (!should_spell(p, w)) {
        if (p->have_cart) {
            zf_char txt[0xc02];
            int r = 1;
            if (zf1_cart_predict(&p->cart, w, txt, 0xc01) == 0) {
                zf1_phonestr_to_ids(p->ps, txt, out, cap);
                r = out[0] ? 0 : 1;
            }
            if (r == 0 && zf1_has_vowel(p->ps, out, vscan < 0 ? u16len(out) : vscan)) {
                *src = 6;
                return 0;
            }
        }
    }
    zf1_pron_spell(p, w, out, cap, 0);
    *src = 8;
    return 0;
}

/* FUN_18005856c: 1 if pron a equals spelled b (strict==0: stress digits skipped, '-' '&' equivalent) */
static int pron_eq_spelled(zf1_pron *p, const uint16_t *a, const uint16_t *b, int strict)
{
    int n1 = u16len(a), n2 = u16len(b), i = 0, j = 0;
    for (;;) {
        uint16_t x, y;
        int skipped = 0;
        if (i >= n1 && j >= n2) return 1;
        x = i < n1 ? a[i] : 0;
        y = j < n2 ? b[j] : 0;
        if (!strict) {
            if (IS_STR1(p, x) || IS_STR2(p, x)) { i++; skipped = 1; }
            if (IS_STR1(p, y) || IS_STR2(p, y)) { j++; continue; }
            if (skipped) continue;
        }
        if (x != y && (!IS_MARK(p, x) || !IS_MARK(p, y))) return 0;
        i++;
        j++;
    }
}

/* ---------------- pass 2: FUN_180047eac ---------------- */
int zf1_pron_pass2(zf1_pron *p, zf_wordlist *wl)
{
    int i;
    uint16_t buf[PH_MAX + 2], sp[PH_MAX + 2];
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        int src = 0, r;
        const zf_char *text = w->text;
        if (w->type != ZF_WT_WORD || !action_ok(w) || zf1_word_curpron(w)) continue;
        memset(buf, 0, sizeof buf);
        if (!text) text = (const zf_char *)L"";
        /* domain "general" branch (1800c2d6c) not reachable for the default domain "none" */
        p->curdom = w->s1b0;
        r = zf1_pron_oov_split(p, w, buf, 0x180);           /* LTA vt+0x28 */
        p->curdom = NULL;
        if (r == 0) src = 0x10;
        if (r == 1) {
            p->curdom = w->s1b0;
            r = zf1_pron_compound(p, text, buf, 0x180);
            p->curdom = NULL;     /* CWordPronouncer mode 8 (COMPOUNDPRON) */
            if (r == 0) src = 9;
        }
        if (r == 1) {
            /* FUN_1800c34f8 / FUN_1800c28cc (no other-language pronouncers for en-US) -> LTS path */
            if (w->s1b0 && streq_a(w->s1b0, "name")) {
                /* name domain: FUN_18008ec9c + RNN LTS (enUS.Name.dat); not implemented */
            }
            r = zf1_pron_lts_word(p, text, buf, 0x180, &src);
        }
        if (r < 0) continue;
        zf1_word_setpron(w, buf, src, 0);
        if (w->i278 != 8) {
            zf1_pron_spell(p, text, sp, 0x180, 0);
            if (buf[0] && pron_eq_spelled(p, buf, sp, 0)) {
                zf1_word_setpron(w, sp, 8, 0);
            }
        }
        w->priv = NULL;
    }
    return 0;
}

/* ---------------- pass 3: CSyllableSeg (FUN_180017d1c) ---------------- */
static int seg_ok(zf1_pron *p, const uint16_t *s, int n)   /* FUN_18004cdb0 */
{
    int v = 0, son = 0, con = 0, i;
    if (n < 1) return 0;
    for (i = 0; i < n; i++) {
        if (IS_VOWEL(p, s[i])) v++;
        if (IS_SON(p, s[i])) son++;
        if (IS_CONS(p, s[i])) con++;
    }
    if (v == 0) return (son != 0 && con != 1);
    return v < 2;
}

int zf1_pron_is_syllabified(zf1_pron *p, const uint16_t *ph, int n)
{
    int st = 0, i, ok = 1;
    for (i = 0; i < n; i++) {
        if (!ok) return 0;
        if (IS_MARK(p, ph[i])) {
            ok = seg_ok(p, ph + st, i - st);
            st = i + 1;
        }
    }
    if (!ok) return 0;
    return seg_ok(p, ph + st, n - st);
}

static int onset_match(zf1_pron *p, const uint16_t *v, int a, int b)   /* FUN_1800cb1d4 */
{
    uint32_t k;
    int i;
    if (!p->nonset || (unsigned)(b - a) >= 5) return 0;
    for (k = 0; k < p->nonset; k++) {
        for (i = a; i < b; i++)
            if (v[i] != zf_rd16(p->onset + 8 * (size_t)k + 2 * (size_t)(i - a))) break;
        if (i == b) return 1;
    }
    return 0;
}

int zf1_pron_syllabify(zf1_pron *p, const uint16_t *ph, int n, uint16_t *out, int cap)
{
    uint16_t v[PH_MAX * 2 + 4];
    int nv = 0, len = 0, i, u, cnt;
    for (i = 0; i < n; i++) if (IS_VOWEL(p, ph[i])) nv++;
    for (i = 0; i < n; i++)
        if (!IS_MARK(p, ph[i]) && len < PH_MAX * 2) v[len++] = ph[i];
    cnt = nv;
    for (u = len - 1; u >= 1; u--) {
        if (IS_VOWEL(p, v[u])) {
            int vi = u;
            while (u > 0 && onset_match(p, v, u - 1, vi)) u--;
            if (u > 0 && cnt > 1 && len < PH_MAX * 2) {
                memmove(v + u + 1, v + u, sizeof(uint16_t) * (size_t)(len - u));
                v[u] = p->sylmark;
                len++;
            }
            cnt--;
        }
    }
    if (len > cap - 1) len = cap - 1;
    memcpy(out, v, sizeof(uint16_t) * (size_t)len);
    out[len] = 0;
    return len;
}

int zf1_pron_pass3(zf1_pron *p, zf_wordlist *wl)
{
    int i;
    uint16_t buf[PH_MAX * 2 + 4];
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        const uint16_t *cur = zf1_word_curpron(w);
        int n;
        if (!cur) continue;
        n = u16len(cur);
        if (zf1_pron_is_syllabified(p, cur, n)) continue;
        zf1_pron_syllabify(p, cur, n, buf, 0x180);
        zf1_word_setpron(w, buf, 0x17, 0);
    }
    return 0;
}

/* ---------------- pass 4: FUN_1800563dc / FUN_180056450 ---------------- */
static int lta_78(const zf_char *text, const zf_char *ne)    /* loc!180010910 */
{
    static const char *const cats[4] = {"sp:datetime", "sp:measurement", "sp:commonTerms", "sp:webUrl"};
    int k;
    const zf_char *s;
    if (!text || !*text) return 1;
    if (streq_a(text, "A") || streq_a(text, "I")) return 1;
    if (ne && *ne)
        for (k = 0; k < 4; k++)
            if (streq_a(ne, cats[k])) return 1;
    for (s = text; *s; s++)
        if (*s == '.') return 1;
    return 0;
}

static int lta_80(const zf_char *text, const zf_char *ne)    /* loc!180019980 */
{
    if (!text || !*text) return 0;
    if (ne && *ne && streq_a(ne, "sp:mixedAcronymWord")) return 1;
    return 0;
}

int zf1_pron_pass4(zf1_pron *p, zf_wordlist *wl)
{
    int i;
    uint16_t sp[PH_MAX + 2];
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        const uint16_t *cur;
        int flag = 0;
        if (w->type == ZF_WT_SILENCE) continue;
        cur = zf1_word_curpron(w);
        if (!w->text || !cur) continue;
        if (!lta_78(w->text, w->ne_type)) {
            if (p->ct.n) {
                zf1_pron_spell(p, w->text, sp, 0x180, 1);
                flag = pron_eq_spelled(p, cur, sp, 0);
            }
            if (!flag) {
                if (lta_80(w->text, w->ne_type)) {
                    zf1_pron_spell(p, w->text, sp, 0x180, 1);
                    zf1_word_setpron(w, sp, 8, 0);
                    flag = 1;
                }
            }
        }
        w->i368 = flag;
    }
    return 0;
}

void zf1_pron_finish(zf1_pron *p, zf_wordlist *wl)
{
    int i;
    for (i = 0; i < wl->n; i++) {
        zf_word *w = &wl->w[i];
        const uint16_t *cur = zf1_word_curpron(w);
        free(w->pron);
        w->pron = cur ? zf1_phones_to_str(p->ps, cur, u16len(cur)) : NULL;
    }
}

int zf1_pron_sentence(zf1_pron *p, zf_wordlist *wl)
{
    zf1_pron_pass1(p, wl);
    zf1_pron_pass2(p, wl);
    if (p->poly) p->poly(p->posobj, wl);
    zf1_pron_pass3(p, wl);
    zf1_pron_pass4(p, wl);
    zf1_pron_finish(p, wl);
    return 0;
}

/* ---------------- <pron sym> ---------------- */
int zf1_pron_sapi(zf1_pron *p, const uint16_t *sapi, zf1_sapiseg *seg, int maxseg)
{
    static const char *const P[] = {"-", "!", "&", ",", ".", "?", "_", "1", "2", "aa", "ae", "ah", "ao", "aw",
                                    "ax", "ay", "b", "ch", "d", "dh", "eh", "er", "ey", "f", "g", "h", "ih", "iy",
                                    "jh", "k", "l", "m", "n", "ng", "ow", "oy", "p", "r", "s", "sh", "t", "th",
                                    "uh", "uw", "v", "w", "y", "z", "zh"};
    uint16_t cur[0x182];
    int nseg = 0, n = 0, i;
    if (maxseg <= 0) return 0;
    for (i = 0;; i++) {
        uint16_t s = sapi ? sapi[i] : 0;
        int brk = (s == 0) || s == 2 || s == 4 || s == 5 || s == 6;
        if (!brk) {
            if (s >= 1 && s <= 49 && s != 7) {
                const char *nm = P[s - 1];
                int id = zf1_phone_id(p->ps, nm, (int)strlen(nm));
                if (id >= 0 && n < 0x180) cur[n++] = (uint16_t)id;
            }
            continue;
        }
        if (n > 0 || s == 0) {
            if (n > 0) {
                zf1_sapiseg *g = &seg[nseg];
                cur[n] = 0;
                g->comma_after = 0;
                if (zf1_pron_is_syllabified(p, cur, n)) { memcpy(g->ph, cur, sizeof(uint16_t) * (size_t)(n + 1)); }
                else zf1_pron_syllabify(p, cur, n, g->ph, 0x182);
                nseg++;
                n = 0;
            }
        }
        if (s == 0 || nseg >= maxseg) break;
        if (nseg > 0) seg[nseg - 1].comma_after = 1;
    }
    if (nseg > 0) seg[nseg - 1].comma_after = 0;
    return nseg;
}

/* lexicon callback for fork B's network runtime (kind-2 labels): one attribute list per attribute set,
 * pairs = parent(id) | id << 16 sorted ascending */
int zf1_pron_netlex_cb(void *ctx, const zf_char *word, int n, uint32_t *pairs, int *counts, int maxitems, int maxpairs)
{
    zf1_pron *p = (zf1_pron *)ctx;
    zf1_lexhit *h = (zf1_lexhit *)malloc(sizeof *h);
    int ni = 0, i, j, k, a, b;
    if (!h) return -1;
    if (!zf1_pron_lookup(p, word, n, h)) { free(h); return -1; }
    for (i = 0; i < h->n; i++)
        for (j = 0; j < h->p[i].nset && ni < maxitems; j++) {
            int c = 0;
            uint32_t *pp = pairs + ni * maxpairs;
            for (k = 0; h->p[i].set[j][k] && c < maxpairs; k++) {
                uint16_t id = h->p[i].set[j][k];
                uint16_t par = id < (uint16_t)p->lx->nattrdefs ? zf_rd16(p->lx->attrdefs + 10 * (size_t)id + 4) : 0xFFFF;
                pp[c++] = (uint32_t)par | ((uint32_t)id << 16);
            }
            for (a = 1; a < c; a++)
                for (b = a; b > 0 && pp[b - 1] > pp[b]; b--) { uint32_t t = pp[b]; pp[b] = pp[b - 1]; pp[b - 1] = t; }
            counts[ni++] = c;
        }
    free(h);
    return ni;
}
