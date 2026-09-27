/* zf1_tn.c - CNEDetector / CTextNormalizer port (see zf1_tn.h, notes/fe1_tn.md).  Portable C99. */
#include "zf1_tn.h"
#include "zf1_fstpm.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ lexicon callback for kind-2 labels */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    /* engine list order: by attribute id (low u16), then value */
    uint32_t kx = ((x & 0xFFFF) << 16) | (x >> 16), ky = ((y & 0xFFFF) << 16) | (y >> 16);
    return kx < ky ? -1 : kx > ky;
}

/* FUN_18013897c + FUN_180035ab0/FUN_1800639a0: one attribute list per attribute set; pair = (parent attribute
 * id, attribute id) for every attribute of the set */
static int tn_lexfn(void *ctx, const zf_char *word, int n, uint32_t *pairs, int *counts, int maxitems, int maxpairs)
{
    zf1_tn *t = (zf1_tn *)ctx;
    zf1_lexent *e;
    int i, j, items = 0;
    if (!t->haslex || n <= 0 || n > 128) return -1;
    if (!t->lexbuf) t->lexbuf = malloc(sizeof(zf1_lexent));
    e = (zf1_lexent *)t->lexbuf;
    if (!e) return -1;
    if (!zf1_lex_lookup(&t->lex, word, n, e)) return -1;
    for (i = 0; i < e->n; i++) {
        for (j = 0; j < e->p[i].nattr && items < maxitems; j++) {
            const uint8_t *ids = zf1_lex_attrset(&t->lex, e->p[i].attrset[j]);
            int k = 0;
            for (; ids && zf_rd16(ids) && k < maxpairs; ids += 2) {
                uint16_t id = zf_rd16(ids), parent = 0xFFFF;
                if (id < t->lex.nattrdefs) parent = zf_rd16(t->lex.attrdefs + 10 * (size_t)id + 4);
                if (parent == 0xFFFF) continue;
                pairs[items * maxpairs + k++] = (uint32_t)parent | ((uint32_t)id << 16);
            }
            qsort(pairs + items * maxpairs, (size_t)k, sizeof(uint32_t), cmp_u32);
            counts[items++] = k;
        }
    }
    return items;
}

int zf1_tn_init(zf1_tn *t, const zf1_dat *d)
{
    size_t n;
    const uint8_t *b;
    memset(t, 0, sizeof *t);
    b = zf1_dat_get(d, 0x7d5841abu, 0xea8d0701u, &n);
    t->net = zf1_net_load(b, n);
    if (!t->net) return -1;
    zf1_net_set_mask(t->net, 1);
    b = zf1_dat_get(d, 0x7bd71f46u, 0xf9a99c02u, &n);
    if (b && !zf1_lex_init(&t->lex, b, n)) t->haslex = 1;
    zf1_net_set_lexicon(t->net, tn_lexfn, t);
    t->posattr = 0;
    b = zf1_dat_get(d, 0xbfc4309du, 0xd2343132u, &n);
    t->fst = b ? zf1_pm_load(b, n) : NULL;
    return 0;
}

void zf1_tn_free(zf1_tn *t)
{
    zf1_net_free(t->net);
    zf1_pm_free((zf1_pm *)t->fst);
    if (t->haslex) zf1_lex_free(&t->lex);
    free(t->lexbuf);
    memset(t, 0, sizeof *t);
}

void zf1_tn_words_free(zf_char **w, int n)
{
    int i;
    for (i = 0; i < n; i++) free(w[i]);
    free(w);
}

/* ------------------------------------------------------------------ Normalize (FUN_1800c3e50) */
static const zf_char SP_DEFAULT[] = {'s', 'p', ':', 'd', 'e', 'f', 'a', 'u', 'l', 't', 0};

int zf1_tn_normalize(zf1_tn *t, const zf_char *text, int len, const zf_char *category, zf_char ***words, int *nwords)
{
    zf1_netres r;
    zf_char *s, *p, *q, **out = NULL;
    int n = 0, cap = 0;
    *words = NULL;
    *nwords = 0;
    if (!t->net || !text || len <= 0 || !category) return 1;
    zf1_net_parse(t->net, text, len, 0, 2, category, 0, &r);
    if (!r.found || r.len < len) {
        zf1_netres_free(&r);
        zf1_net_parse(t->net, text, len, 0, 2, SP_DEFAULT, 0, &r);
    }
    if (!r.found || r.len != len) {
        zf1_netres_free(&r);
        return 1;
    }
    if (r.nores) {
        /* MultiResult found no consistent path (0x80041004): the original text is kept (1800c3e50) */
        s = zf_strndup(text, (size_t)len);
    } else {
        s = zf1_netres_text(&r);
    }
    zf1_netres_free(&r);
    /* split on ' ' (1800c40b0 loop: empty pieces are skipped) */
    for (p = s;;) {
        q = p;
        while (*q && *q != ' ') q++;
        if (q > p) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                out = (zf_char **)realloc(out, sizeof(zf_char *) * (size_t)cap);
            }
            out[n++] = zf_strndup(p, (size_t)(q - p));
        }
        if (!*q) break;
        p = q + 1;
    }
    free(s);
    *words = out;
    *nwords = n;
    return 0;
}

/* ------------------------------------------------------------------ FindNext (FUN_180061910) */

/* the FST's tag "sp:cat%COST=n%POS=n%NOFST=x" (FUN_1800cf5ac): category = text before the first '%' */
static void tag_fields(const char *tag, int *cost, int *pos, int *nofst)
{
    const char *p = tag;
    while ((p = strchr(p, '%')) != NULL) {
        const char *eq;
        p++;
        eq = strchr(p, '=');
        if (!eq) break;
        if (!strncmp(p, "COST", 4)) *cost = atoi(eq + 1);
        else if (!strncmp(p, "POS", 3)) *pos = atoi(eq + 1);
        else if (!strncmp(p, "NOFST", 5)) *nofst = atoi(eq + 1);
    }
}

/* NE FST pre-pass (text without a say-as category): 0 = entity, 1 = no entity, 2 = ask the network.
 * The matcher runs on text[base..len) (FUN_180075900: fails on non-ASCII text -> network); of the matches at
 * the first matching offset the one with the lowest COST is taken (FUN_180061f08: first strict minimum); none ->
 * no entity; NOFST=1 -> network from the start; an entity that does not start and end on lattice token
 * boundaries (tokenizer 3) -> the matcher is re-run from the next token start after its offset. */
static int ne_fst(zf1_tn *t, const zf_char *text, int len, int *pre, int *elen, zf_char *cat, int catcap,
                  uint16_t *attr)
{
    zf1_pm_match m[32];
    char *buf = (char *)malloc((size_t)len + 1);
    int base = 0, rc = 1;
    for (;;) {
        int i, k, best = -1, bcost = 0, bpos = 0, bno = 0, o, l, step;
        unsigned lowest = 0xFFFFFFFFu;
        for (i = base; i < len; i++) {
            if (text[i] >= 0x80 || text[i] == 0) break;
            buf[i - base] = (char)text[i];
        }
        if (i < len) { rc = 2; break; }
        k = zf1_pm_next((zf1_pm *)t->fst, buf, len - base, 0, m, 32);
        if (k <= 0) { rc = 1; break; }
        for (i = 0; i < k; i++) {
            int cost = 0, pos = 0, nofst = 0;
            tag_fields(m[i].tag, &cost, &pos, &nofst);
            if ((unsigned)cost < lowest) {
                lowest = (unsigned)cost;
                best = i;
                bcost = cost;
                bpos = pos;
                bno = nofst;
            }
        }
        (void)bcost;
        /* FUN_180061f08 stores a non-zero POS of each iteration's best match in the caller's variable at once,
         * so a candidate rejected below still leaves its POS behind */
        if (attr && bpos != 0) *attr = (uint16_t)bpos;
        if (bno != 0) { rc = 2; break; }
        o = m[best].off;
        l = m[best].len;
        if (l == 0) { rc = 1; break; }
        if (zf1_lat_start_ok(3, text, len, base + o) && zf1_lat_end_ok(3, text, len, base + o + l)) {
            if (pre) *pre = base + o;
            if (elen) *elen = l;
            if (cat && catcap) {
                for (i = 0; m[best].tag[i] && m[best].tag[i] != '%' && i < catcap - 1; i++) cat[i] = (zf_char)m[best].tag[i];
                cat[i] = 0;
            }
            rc = 0;
            break;
        }
        step = o;
        do {
            step++;
        } while (!zf1_lat_start_ok(3, text, len, base + step) && base + step < len);
        base += step;
        if (base >= len) { rc = 1; break; }
    }
    free(buf);
    return rc;
}

int zf1_ne_findnext(zf1_tn *t, const zf_char *text, int len, const zf_char *category, int *pre, int *elen,
                    zf_char *cat, int catcap, uint16_t *attr)
{
    int pos = 0, i, allws = 1;
    if (cat && catcap) cat[0] = 0;
    if (attr) *attr = 0xFFFF;
    if (!t->net || !text || len <= 0) return 1;
    for (i = 0; i < len; i++)
        if (!zf1_iswspace(text[i])) { allws = 0; break; }
    if (allws) return 1;
    if (!category && t->fst) {
        int rc = ne_fst(t, text, len, pre, elen, cat, catcap, attr);
        if (rc != 2) return rc;
        if (cat && catcap) cat[0] = 0;
        if (attr) *attr = 0xFFFF;
    }
    while (pos < len) {
        zf1_netres r;
        int step = 0;
        zf1_net_parse(t->net, text, len, pos, category ? 2 : 1, category, 3, &r);
        if (r.err) { zf1_netres_free(&r); return 1; }
        if (r.found && r.len) {
            if (!r.nores && r.category && r.category[0]) {
                if (pre) *pre = pos;
                if (elen) *elen = r.len;
                if (cat && catcap) {
                    int k = 0;
                    while (r.category[k] && k < catcap - 1) { cat[k] = r.category[k]; k++; }
                    cat[k] = 0;
                }
                if (attr) {
                    for (i = 0; i < r.nattrs; i++)
                        if ((uint16_t)(r.attrs[i] & 0xFFFF) == t->posattr) { *attr = (uint16_t)(r.attrs[i] >> 16); break; }
                }
                zf1_netres_free(&r);
                return 0;
            }
            step = r.len;
        }
        zf1_netres_free(&r);
        /* lattice vt+8 (FUN_180139680): skip the parsed length, else to the next token start */
        pos = step ? pos + step : zf1_lat_next(3, text, len, pos);
    }
    return 1;
}

/* ------------------------------------------------------------------ say-as category (FUN_1800c4f10 / FUN_1800c6aec) */
int zf1_tn_sayas_category(zf1_tn *t, const zf_char *interpret_as, const zf_char *format, zf_char *out, int cap)
{
    int k = 0, i;
    if (out && cap) out[0] = 0;
    if (!interpret_as || !interpret_as[0] || !out || cap < 8) return 0;
    out[k++] = 'm'; out[k++] = 'l'; out[k++] = ':';
    for (i = 0; interpret_as[i] && k < cap - 1; i++) out[k++] = interpret_as[i];
    if (format && format[0] && k < cap - 1) {
        out[k++] = '_';
        for (i = 0; format[i] && k < cap - 1; i++) out[k++] = format[i];
    }
    out[k] = 0;
    if (zf1_net_category_supported(t->net, out, 2)) return 1;
    out[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ fragment segmentation (FUN_1800c6888) */
int zf1_tn_segment(zf1_tn *t, const zf_char *text, int len, const zf_char *sayas, zf1_tnseg **segs)
{
    int pos = 0, n = 0, cap = 0;
    zf1_tnseg *out = NULL;
    zf_char scat[128];
    const zf_char *category = NULL;
    *segs = NULL;
    if (sayas && zf1_tn_sayas_category(t, sayas, NULL, scat, 128)) category = scat;
    while (pos < len) {
        int pre = 0, el = 0, rc;
        zf_char cat[64];
        uint16_t attr;
        rc = zf1_ne_findnext(t, text + pos, len - pos, category, &pre, &el, cat, 64, &attr);
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 8;
            out = (zf1_tnseg *)realloc(out, sizeof(zf1_tnseg) * (size_t)cap);
        }
        if (rc != 0) {
            out[n].off = pos; out[n].len = len - pos; out[n].entity = 0; out[n].cat[0] = 0; n++;
            break;
        }
        if (pre > 0) {
            out[n].off = pos; out[n].len = pre; out[n].entity = 0; out[n].cat[0] = 0; n++;
        }
        out[n].off = pos + pre; out[n].len = el; out[n].entity = 1;
        memcpy(out[n].cat, cat, sizeof cat);
        out[n].attr = attr;
        n++;
        pos += pre + el;
        if (el <= 0) break;
    }
    *segs = out;
    return n;
}

/* ------------------------------------------------------------------ domain of a word (FUN_18002bf08) */
static int starts_with_a(const zf_char *s, const char *p)
{
    while (*p) { if (*s != (unsigned char)*p) return 0; s++; p++; }
    return 1;
}

void zf1_tn_domain(zf_word *w)
{
    const zf_char *key = w->fs.sayas;
    if (key && key[0]) {
        const char *d = NULL;
        if (!zf_strcmp_a(key, "number_digit") || !zf_strcmp_a(key, "number:digit") || !zf_strcmp_a(key, "cardinal") ||
            !zf_strcmp_a(key, "number:cardinal")) d = "cardinal";
        else if (starts_with_a(key, "date")) d = "date";
        else if (starts_with_a(key, "time")) d = "time";
        else if (!zf_strcmp_a(key, "telephone") || !zf_strcmp_a(key, "phone_number")) d = "phonenumber";
        else if (!zf_strcmp_a(key, "net_email") || !zf_strcmp_a(key, "net:email")) d = "url";
        if (d) zf_setstr_a(&w->s1b0, d);
        else {
            /* FUN_1800720a4: lower-cased copy of the key (towlower, C locale) */
            size_t n = zf_strlen(key), i;
            zf_char *c = zf_strndup(key, n);
            for (i = 0; i < n; i++) if (c[i] >= 'A' && c[i] <= 'Z') c[i] += 32;
            zf_setstr(&w->s1b0, c);
            free(c);
        }
    }
    if ((!w->s1b0 || !zf_strcmp_a(w->s1b0, "none")) && w->ne_type && starts_with_a(w->ne_type, "sp:")) {
        static const char *const M[][2] = {{"sp:cardinal", "cardinal"}, {"sp:date", "date"}, {"sp:time", "time"},
            {"sp:phoneNumber", "phonenumber"}, {"sp:address", "address"}, {"sp:email", "url"}, {"sp:webUrl", "url"},
            {"sp:filePath", "url"}, {"sp:message", "message"}, {"sp:name", "name"}};
        size_t i;
        for (i = 0; i < sizeof M / sizeof M[0]; i++)
            if (!zf_strcmp_a(w->ne_type, M[i][0])) { zf_setstr_a(&w->s1b0, M[i][1]); break; }
    }
}

/* ------------------------------------------------------------------ Analyze step 7 (FUN_18004b06c) */
static void set_text(zf_word *w, const zf_char *s, size_t n)
{
    size_t i;
    free(w->text); free(w->ci); free(w->regular);
    w->text = zf_strndup(s, n);
    w->ci = zf_strndup(s, n);
    w->regular = zf_strndup(s, n);
    for (i = 0; i < n; i++) {
        zf_char c = s[i];
        w->ci[i] = (c >= 'a' && c <= 'z') ? (zf_char)(c - 32) : c;   /* towupper, C locale */
        w->regular[i] = zf_tolower(c);
    }
}

static int parse_break(const zf_char *s, int *ms, const zf_char **after, const zf_char **bstart)
{
    /* wcsstr(s, "[break=") then wcsstr(from there, "ms]") */
    static const zf_char B[] = {'[', 'b', 'r', 'e', 'a', 'k', '=', 0};
    const zf_char *p, *q;
    for (p = s; *p; p++) {
        int k = 0;
        while (B[k] && p[k] == B[k]) k++;
        if (!B[k]) break;
    }
    if (!*p) return 0;
    for (q = p; *q; q++)
        if (q[0] == 'm' && q[1] == 's' && q[2] == ']') break;
    if (!*q) return 0;
    {
        const zf_char *d = p + 7;
        int v = 0, neg = 0;
        while (d < q && (*d == ' ' || *d == '\t')) d++;
        if (d < q && (*d == '-' || *d == '+')) { neg = *d == '-'; d++; }
        while (d < q && *d >= '0' && *d <= '9') { v = v * 10 + (*d - '0'); d++; }
        *ms = neg ? -v : v;
    }
    *bstart = p;
    *after = q + 3;
    return 1;
}

int zf1_tn_expand(zf1_tn *t, zf_wordlist *wl, zf1_tn_postag_fn postag, void *ctx)
{
    int i;
    for (i = 0; i < wl->n; i++) {
        zf_word *tok = &wl->w[i];
        zf_char **outw;
        int nout, rc, k, created = 0, first;
        uint16_t tok_pos;
        zf_wordlist nw;
        if (!tok->ne_type || !tok->ne_type[0] || !tok->text) continue;
        rc = zf1_tn_normalize(t, tok->text, (int)zf_strlen(tok->text), tok->ne_type, &outw, &nout);
        if (rc != 0) continue;   /* S_FALSE: keep the token */
        memset(&nw, 0, sizeof nw);
        for (k = 0; k < nout; k++) {
            const zf_char *s = outw[k];
            while (*s) {
                int ms = 0;
                const zf_char *after = NULL, *bs = NULL;
                if (parse_break(s, &ms, &after, &bs)) {
                    if (bs > s) {
                        /* text before the marker becomes a raw token (FUN_18006e874) */
                        zf_word *w = zf_wl_push(&nw);
                        w->fs = tok->fs;
                        w->lang = tok->lang;
                        w->src_off = tok->src_off; w->src_len = tok->src_len;
                        set_text(w, s, (size_t)(bs - s));
                        w->i27c = 1;
                        zf_setstr(&w->ne_type, tok->ne_type);
                        zf_setstr(&w->s1b0, tok->s1b0);
                        created++;
                    }
                    if (nw.n) {
                        zf_word *pw = &nw.w[nw.n - 1];
                        pw->i214 += ms;
                        pw->i248 = 0;
                    }
                    s = after;
                } else {
                    zf_word *w = zf_wl_push(&nw);
                    w->fs = tok->fs;
                    w->lang = tok->lang;
                    w->src_off = tok->src_off; w->src_len = tok->src_len;
                    w->type = ZF_WT_WORD;
                    set_text(w, s, zf_strlen(s));
                    if (tok->i2f0) {   /* grouping info moves to the first new word (+0x2f0/+0x2f8/+0x318) */
                        w->i2f0 = tok->i2f0;
                        zf_setstr(&w->s2f8, tok->s2f8);
                        tok->i2f0 = 0;
                        free(tok->s2f8); tok->s2f8 = NULL;
                    }
                    w->i27c = 1;
                    zf_setstr(&w->ne_type, tok->ne_type);
                    zf_setstr(&w->s1b0, tok->s1b0);
                    if (k + 1 < nout) {
                        if (!w->bi_lock) w->bi = 2;
                        w->i248 = 1;
                    }
                    created++;
                    break;
                }
            }
        }
        zf1_tn_words_free(outw, nout);
        tok_pos = tok->pos;
        if (nw.n == 1) nw.w[0].pos = tok->pos;   /* vt+0xe8(old POS) */
        /* splice: replace tok by the new words */
        zf_word_free(tok);
        memmove(wl->w + i, wl->w + i + 1, sizeof(zf_word) * (size_t)(wl->n - i - 1));
        wl->n--;
        first = i;
        for (k = 0; k < nw.n; k++) {
            zf_word *d = zf_wl_insert(wl, i + k);
            zf_word_free(d);
            *d = nw.w[k];
        }
        free(nw.w);
        if (postag && nw.n) postag(ctx, wl, first, nw.n, tok_pos);
        i += nw.n - 1;
        (void)created;
    }
    return 0;
}
