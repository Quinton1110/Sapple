/* zf1_frag.c - SAPI XML parsing (behaviour of sapi.dll, same as the Anna port's anna_sapixml.h, verified against
 * the F lines of harness/zftap1.exe) and the engine's fragment node conversion.  Portable C99. */
#include "zf1_frag.h"
#include <stdlib.h>
#include <string.h>

typedef zf_char wc;

typedef struct xmem { struct xmem *next; wc data[1]; } xmem;

typedef struct vstate {
    int action, emph, rate, vol, pitch, range, sil;
    const uint16_t *phones;
    int pos;
    const wc *ctx;
} vstate;
static const vstate DEFAULT_STATE = {0, 0, 0, 100, 0, 0, 0, NULL, 0, NULL};

static wc *xalloc(zf1_frags *f, int n)
{
    xmem *m = (xmem *)calloc(1, sizeof(xmem) + sizeof(wc) * (size_t)n);
    if (!m) return NULL;
    m->next = (xmem *)f->xmlmem;
    f->xmlmem = m;
    return m->data;
}

static int wnicmp_a(const wc *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int x = a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x - y;
        if (!y) return 0;
    }
    return 0;
}
static int wncmp_a(const wc *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != (unsigned char)b[i]) return (int)a[i] - (unsigned char)b[i];
        if (!b[i]) return 0;
    }
    return 0;
}
static int x_isws(wc c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int add_frag(zf1_frags *f, const vstate *st, int start, int len, int ofs)
{
    zf1_sfrag *x = (zf1_sfrag *)realloc(f->sf, sizeof(zf1_sfrag) * (size_t)(f->nsf + 1)), *s;
    if (!x) return -1;
    f->sf = x;
    s = &f->sf[f->nsf++];
    memset(s, 0, sizeof *s);
    s->action = st->action; s->emph = st->emph; s->rate = st->rate; s->vol = st->vol;
    s->pitch = st->pitch; s->range = st->range; s->sil = st->sil; s->phones = st->phones;
    s->pos = st->pos; s->ctx = st->ctx;
    s->text = f->buf + start;
    s->len = len;
    s->ofs = ofs;
    s->group = f->nsf > 1 ? f->sf[f->nsf - 2].group + (f->group_next ? 1 : 0) : 0;
    f->group_next = 0;
    return 0;
}

/* SAPI en-US phone set (SpPhoneConverter, LangID 0x409) */
static int phone_id(const wc *s, int n)
{
    static const char *const P[] = {"-", "!", "&", ",", ".", "?", "_", "1", "2", "aa", "ae", "ah", "ao", "aw",
                                    "ax", "ay", "b", "ch", "d", "dh", "eh", "er", "ey", "f", "g", "h", "ih", "iy",
                                    "jh", "k", "l", "m", "n", "ng", "ow", "oy", "p", "r", "s", "sh", "t", "th",
                                    "uh", "uw", "v", "w", "y", "z", "zh"};
    int k;
    for (k = 0; k < (int)(sizeof P / sizeof *P); k++)
        if ((int)strlen(P[k]) == n && !wnicmp_a(s, P[k], n)) return k + 1;
    return -1;
}

/* PhoneIDs are written over the sym attribute value in the buffer (0-terminated), as sapi.dll does */
static const uint16_t *phones_parse(wc *s, int n)
{
    wc *out = s;
    int i = 0, k = 0;
    while (i < n) {
        int j, id;
        while (i < n && x_isws(s[i])) i++;
        j = i;
        while (j < n && !x_isws(s[j])) j++;
        if (j > i) {
            id = phone_id(s + i, j - i);
            if (id > 0) out[k++] = (wc)id;
        }
        i = j;
    }
    out[k] = 0;
    return out;
}

static void x_nul_attrs(wc *t, const wc *end)
{
    wc *p = t;
    while (p < end) {
        if (*p == '=') {
            wc *q = p + 1, quote;
            while (q < end && x_isws(*q)) q++;
            quote = q < end ? *q : 0;
            if (quote == '"' || quote == '\'') {
                q++;
                while (q < end && *q != quote) q++;
                if (q < end) *q = 0;
                p = q;
            }
        }
        p++;
    }
}

static const wc *x_attr(const wc *t, const wc *end, const char *name, int *len)
{
    int nl = (int)strlen(name);
    const wc *p = t;
    while (p < end) {
        if ((p == t || x_isws(p[-1])) && end - p > nl && !wnicmp_a(p, name, nl)) {
            const wc *q = p + nl;
            while (q < end && x_isws(*q)) q++;
            if (q < end && *q == '=') {
                wc quote;
                q++;
                while (q < end && x_isws(*q)) q++;
                quote = *q;
                if (quote == '"' || quote == '\'') {
                    const wc *v = ++q;
                    while (q < end && *q != quote) q++;
                    *len = (int)(q - v);
                    return v;
                }
            }
        }
        p++;
    }
    return NULL;
}

static long x_long(const wc *v, int n)
{
    long x = 0;
    int i = 0, neg = 0;
    while (i < n && x_isws(v[i])) i++;
    if (i < n && (v[i] == '-' || v[i] == '+')) neg = v[i++] == '-';
    while (i < n && v[i] >= '0' && v[i] <= '9') x = x * 10 + (v[i++] - '0');
    return neg ? -x : x;
}

static int x_name_is(const wc *s, int n, const char *name) { return (int)strlen(name) == n && !wnicmp_a(s, name, n); }

static void x_entities(wc *s, int n)
{
    static const struct { const char *e; wc c; } E[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    int i;
    for (i = 0; i < n; i++) {
        size_t k;
        if (s[i] != '&') continue;
        if (i + 3 < n && s[i + 1] == '#' && (s[i + 2] == 'x' || s[i + 2] == 'X')) {
            int j = i + 3;
            unsigned v = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || (s[j] >= 'a' && s[j] <= 'f') || (s[j] >= 'A' && s[j] <= 'F'))) {
                v = v * 16 + (unsigned)(s[j] <= '9' ? s[j] - '0' : (s[j] | 0x20) - 'a' + 10);
                j++;
            }
            if (j > i + 3 && j < n && s[j] == ';') {
                int q;
                s[i] = 0x200b;
                s[i + 1] = (wc)v;
                for (q = i + 2; q < j; q++) s[q] = 0x200b;
                i = j;
                continue;
            }
        }
        for (k = 0; k < sizeof E / sizeof *E; k++) {
            int l = (int)strlen(E[k].e), j;
            if (i + l <= n && !wncmp_a(s + i, E[k].e, l)) {
                s[i] = E[k].c;
                for (j = 1; j < l; j++) s[i + j] = 0x200b;
                i += l - 1;
                break;
            }
        }
    }
}

static int sapi_xml_parse(zf1_frags *f)
{
    wc *b = f->buf;
    int n = f->n, i = 0, sp = 0;
    vstate stack[64], cur = DEFAULT_STATE;
    while (i < n) {
        if (b[i] == '<') {
            int s = i, close = 0, selfclose = 0, ns, nn, ts, te, known = 1;
            const wc *nm0;
            if (i + 4 <= n && !wncmp_a(b + i, "<!--", 4)) {
                int j = i + 4;
                while (j + 3 <= n && wncmp_a(b + j, "-->", 3)) j++;
                i = j + 3 <= n ? j + 3 : n;
                while (i < n && x_isws(b[i])) i++;
                continue;
            }
            if (i + 9 <= n && !wncmp_a(b + i, "<![CDATA[", 9)) {
                int j = i + 9;
                vstate st = cur;
                while (j + 3 <= n && wncmp_a(b + j, "]]>", 3)) j++;
                j = j + 3 <= n ? j + 3 : n;
                while (j < n && x_isws(b[j])) j++;
                st.action = 6;
                if (add_frag(f, &st, s, j - s, s) < 0) return -1;
                i = j;
                continue;
            }
            te = i + 1;
            while (te < n && b[te] != '>') te++;
            if (te >= n) return -2;     /* unterminated tag: sapi.dll rejects the text */
            ts = i + 1;
            if (ts < te && b[ts] == '/') { close = 1; ts++; }
            if (te > ts && b[te - 1] == '/') selfclose = 1;
            ns = ts;
            while (ns < te && !x_isws(b[ns]) && b[ns] != '/') ns++;
            nn = ns - ts;
            i = te < n ? te + 1 : n;
            nm0 = b + ts;
            if (x_name_is(nm0, nn, "volume") || x_name_is(nm0, nn, "rate") || x_name_is(nm0, nn, "pitch") ||
                x_name_is(nm0, nn, "emph") || x_name_is(nm0, nn, "spell") || x_name_is(nm0, nn, "context") ||
                x_name_is(nm0, nn, "partofsp") || x_name_is(nm0, nn, "voice") || x_name_is(nm0, nn, "lang") ||
                x_name_is(nm0, nn, "sapi")) {
                if (x_name_is(nm0, nn, "voice") || x_name_is(nm0, nn, "lang")) f->group_next = 1;
                if (close) {
                    /* sapi.dll rejects the whole text when an end tag closes nothing; a mismatched end tag just
                       closes the innermost element */
                    if (sp <= 0) return -2;
                    cur = stack[--sp];
                } else {
                    const wc *v;
                    int vl;
                    vstate nst = cur;
                    if (x_name_is(nm0, nn, "volume") && (v = x_attr(b + ns, b + te, "level", &vl)) != NULL) {
                        long x = x_long(v, vl);
                        nst.vol = x < 0 ? 0 : x > 100 ? 100 : (int)x;
                    } else if (x_name_is(nm0, nn, "rate")) {
                        if ((v = x_attr(b + ns, b + te, "absspeed", &vl)) != NULL) nst.rate = (int)x_long(v, vl);
                        else if ((v = x_attr(b + ns, b + te, "speed", &vl)) != NULL) nst.rate += (int)x_long(v, vl);
                    } else if (x_name_is(nm0, nn, "pitch")) {
                        if ((v = x_attr(b + ns, b + te, "absmiddle", &vl)) != NULL) nst.pitch = (int)x_long(v, vl);
                        else if ((v = x_attr(b + ns, b + te, "middle", &vl)) != NULL) nst.pitch += (int)x_long(v, vl);
                        if ((v = x_attr(b + ns, b + te, "absrange", &vl)) != NULL) nst.range = (int)x_long(v, vl);
                        else if ((v = x_attr(b + ns, b + te, "range", &vl)) != NULL) nst.range += (int)x_long(v, vl);
                    } else if (x_name_is(nm0, nn, "emph")) {
                        nst.emph = 1;
                    } else if (x_name_is(nm0, nn, "spell")) {
                        nst.action = 4;
                    } else if (x_name_is(nm0, nn, "context") && (v = x_attr(b + ns, b + te, "id", &vl)) != NULL) {
                        wc *c = xalloc(f, vl + 1);
                        if (!c) return -1;
                        memcpy(c, v, sizeof(wc) * (size_t)vl);
                        c[vl] = 0;
                        nst.ctx = c;
                    } else if (x_name_is(nm0, nn, "partofsp") && (v = x_attr(b + ns, b + te, "part", &vl)) != NULL) {
                        static const struct { const char *n; int pos; } PS[] = {
                            {"noun", 0x1000}, {"verb", 0x2000}, {"modifier", 0x3000},
                            {"function", 0x4000}, {"interjection", 0x5000}, {"unknown", 0}};
                        size_t k;
                        for (k = 0; k < sizeof PS / sizeof *PS; k++)
                            if (x_name_is(v, vl, PS[k].n)) nst.pos = PS[k].pos;
                    }
                    if (!selfclose && sp < 64) stack[sp++] = cur;
                    cur = nst;
                }
            } else if (x_name_is(nm0, nn, "silence")) {
                if (!close) {
                    const wc *v;
                    int vl;
                    vstate st = cur;
                    long x = (v = x_attr(b + ns, b + te, "msec", &vl)) != NULL ? x_long(v, vl) : 0;
                    st.action = 1;
                    st.sil = x < 0 ? 0 : (int)x;
                    if (add_frag(f, &st, s, 0, s) < 0) return -1;
                    f->sf[f->nsf - 1].text = NULL;
                }
            } else if (x_name_is(nm0, nn, "bookmark")) {
                if (!close) {
                    const wc *v;
                    int vl = 0;
                    vstate st = cur;
                    v = x_attr(b + ns, b + te, "mark", &vl);
                    st.action = 3;
                    if (add_frag(f, &st, v ? (int)(v - b) : s, vl, s) < 0) return -1;
                }
            } else if (x_name_is(nm0, nn, "pron")) {
                if (close) {
                    if (sp <= 0) return -2;
                    cur = stack[--sp];
                } else {
                    const wc *v;
                    int vl = 0;
                    vstate st = cur;
                    v = x_attr(b + ns, b + te, "sym", &vl);
                    st.action = 2;
                    st.phones = v ? phones_parse((wc *)v, vl) : NULL;
                    if (selfclose) {
                        if (add_frag(f, &st, s, 0, s) < 0) return -1;
                        f->sf[f->nsf - 1].text = NULL;
                    } else {
                        if (sp < 64) stack[sp++] = cur;
                        cur = st;
                    }
                }
            } else {
                known = 0;
            }
            if (known && !close && (x_name_is(nm0, nn, "context") || x_name_is(nm0, nn, "pron")))
                x_nul_attrs(b + ns, b + te);
            if (!known) {
                vstate st = cur;
                int j = i;
                while (j < n && x_isws(b[j])) j++;
                st.action = 6;
                if (add_frag(f, &st, s, j - s, s) < 0) return -1;
                i = j;
            } else {
                while (i < n && x_isws(b[i])) i++;
            }
            continue;
        }
        {
            int j = i, k, end;
            while (j < n && b[j] != '<') j++;
            x_entities(b + i, j - i);
            k = i;
            while (k < j && x_isws(b[k])) k++;
            end = j;
            if (j == n)
                while (end > k && x_isws(b[end - 1])) end--;
            if (k < end && add_frag(f, &cur, k, end - k, k) < 0) return -1;
            i = j;
        }
    }
    return 0;
}

uint16_t zf1_sapi_pos(int p)
{
    /* engine POS converter (engine vt+0x18); verified: noun 0x1000 -> 3, verb 0x2000 -> 8 */
    switch (p & 0xF000) {
    case 0x1000: return 3;
    case 0x2000: return 8;
    case 0x3000: return 11;     /* modifier */
    case 0x4000: return 32;     /* function */
    case 0x5000: return 38;     /* interjection */
    default: return 0xFFFF;
    }
}

/* FUN_18008f8dc: isspace in the "C" locale for chars < 0x100 */
static int c_isspace(zf_char c) { return c == ' ' || (c >= 9 && c <= 13); }

static int build_nodes(zf1_frags *f)
{
    int i;
    f->nd = (zf1_node *)calloc((size_t)(f->nsf ? f->nsf : 1), sizeof(zf1_node));
    if (!f->nd) return -1;
    f->nnd = 0;
    for (i = 0; i < f->nsf; i++) {
        const zf1_sfrag *s = &f->sf[i];
        zf1_node *nd;
        int len = s->len, k, m;
        zf_char *copy = NULL;
        /* FUN_18008c26c: copy without U+200B for Speak/SpellOut/ParseUnknownTag */
        /* The engine only copies the text when it contains U+200B; otherwise the node points into the SAPI text
         * and the character after the fragment (read by the sentence separator's tokenizer) is the next source
         * character.  Our copy keeps that character at text[len] (0 when the engine made a copy). */
        {
            int has_zw = 0;
            zf_char follow = 0;
            if (s->text) {
                for (k = 0; k < len; k++) if (s->text[k] == 0x200b) has_zw = 1;
                follow = s->text[len];
            }
            if ((s->action & ~6) == 0 && s->action != 2 && s->text && has_zw) {
                copy = (zf_char *)malloc(sizeof(zf_char) * ((size_t)len + 2));
                for (k = 0, m = 0; k < len; k++)
                    if (s->text[k] != 0x200b) copy[m++] = s->text[k];
                copy[m] = 0;
                copy[m + 1] = 0;
                len = m;
            } else if (s->text) {
                copy = (zf_char *)malloc(sizeof(zf_char) * ((size_t)len + 2));
                memcpy(copy, s->text, sizeof(zf_char) * (size_t)len);
                copy[len] = follow;
                copy[len + 1] = 0;
            }
        }
        if (s->action == 6) {   /* ParseUnknownTag: only <lexicon> is examined, the fragment is dropped */
            free(copy);
            continue;
        }
        nd = &f->nd[f->nnd++];
        memset(nd, 0, sizeof *nd);
        nd->lang = 0x409;
        nd->pos = 0xFFFF;
        nd->text = copy;
        nd->len = len;
        nd->src_off = s->ofs;
        nd->group = s->group;
        /* FUN_18008bb98 */
        switch (s->action) {
        case 0: case 2: case 4:
            nd->action = s->action;
            if (s->emph) nd->emph = 1;
            if (s->pos) nd->pos = zf1_sapi_pos(s->pos);
            if (s->pitch != 0 || s->rate != 0 || s->vol != 100) {
                nd->has_prosody = 1;
                nd->pitch = (float)s->pitch;
                nd->rate = (float)s->rate;
                nd->vol = (float)(unsigned)s->vol;
            }
            if (s->action != 2 && s->ctx) nd->sayas = s->ctx;
            if (s->action == 2) nd->sapi_phones = s->phones;
            break;
        case 1: nd->action = 1; nd->silence_ms = s->sil; break;
        case 3: nd->action = 3; break;
        case 5: nd->action = 5; break;
        default: nd->action = 8; break;
        }
    }
    /* FUN_18008d958: trim ASCII blanks of say-as fragments */
    for (i = 0; i < f->nnd; i++) {
        zf1_node *nd = &f->nd[i];
        int a, e;
        if (!nd->sayas || !nd->len || !nd->text) continue;
        a = 0;
        while (a <= nd->len - 1 && c_isspace(nd->text[a])) a++;
        e = nd->len - 1;
        while (e > a && c_isspace(nd->text[e])) e--;
        if (a == nd->len) { nd->len = 0; continue; }
        {
            zf_char follow = nd->text[e + 1];   /* e + 1 <= len: first trimmed char, or the follow char */
            memmove(nd->text, nd->text + a, sizeof(zf_char) * (size_t)(e - a + 1));
            nd->text[e - a + 1] = follow;
            nd->text[e - a + 2] = 0;
        }
        nd->src_off += a;
        nd->len = e - a + 1;
    }
    return 0;
}

int zf1_frags_build(zf1_frags *f, const zf_char *text, int len, int xml)
{
    memset(f, 0, sizeof *f);
    if (len < 0) len = (int)zf_strlen(text);
    f->buf = zf_strndup(text, (size_t)len);
    f->n = len;
    if (!f->buf) return -1;
    if (xml < 0) {      /* SPF_DEFAULT: XML when the first non-blank character is '<' */
        int k = 0;
        while (k < len && x_isws(text[k])) k++;
        xml = (k < len && text[k] == '<');
    }
    if (xml) {
        if (sapi_xml_parse(f) < 0) return -1;
    } else if (len > 0) {
        /* sapi.dll skips leading blanks of plain text (trailing ones are kept) */
        int k = 0;
        while (k < len && (x_isws(f->buf[k]) || f->buf[k] == 0x200b)) k++;
        if (k < len && add_frag(f, &DEFAULT_STATE, k, len - k, k) < 0) return -1;
    }
    return build_nodes(f);
}

void zf1_frags_free(zf1_frags *f)
{
    xmem *m = (xmem *)f->xmlmem;
    int i;
    while (m) { xmem *nx = m->next; free(m); m = nx; }
    for (i = 0; i < f->nnd; i++) free(f->nd[i].text);
    free(f->nd);
    free(f->sf);
    free(f->buf);
    memset(f, 0, sizeof *f);
}
