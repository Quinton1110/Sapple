/* SAPI 5.3 XML -> SPVTEXTFRAG list, as sapi.dll hands it to the engine (included by anna_norm.c).
 *
 * Behaviour matched against sapi.dll with harness/annatap_norm.exe (F lines), see notes/norm.md:
 *   - the fragment text points into a copy of the XML string; entities (&amp; &lt; &gt; &quot; &apos;) are
 *     decoded in place and padded with U+200B (which the engine treats as whitespace)
 *   - whitespace-only text between tags is dropped; leading whitespace of a text run is skipped; trailing
 *     whitespace is kept, except at the end of the input
 *   - <volume level> absolute, clamped to 0..100; <rate speed> and <pitch middle/range> add to the current
 *     value (no clamping), absspeed/absmiddle/absrange set it; <emph> sets EmphAdj = 1; an empty-element tag
 *     (<rate speed="2"/>) changes the state for the rest of the text
 *   - <silence msec> -> eAction 1 (no text, offset of the tag); <bookmark mark> -> eAction 3 (text = mark);
 *     <pron sym> -> eAction 2 with PhoneIDs (text = content, or none for an empty element);
 *     <spell> -> eAction 4; <context id> -> Context.pCategory (as written); <partofsp part> -> ePartOfSpeech
 *   - <voice>, <lang>, <sapi> make no fragment, but <voice> and <lang> end the engine call: the text after
 *     them goes to the engine in a new Speak() call (a new fragment group, so a sentence ends there);
 *     comments vanish; unknown tags and CDATA become eAction 6 fragments holding the tag text plus the
 *     whitespace after it
 *   - hex character references "&#x41;" become U+200B, 'A', U+200B..., ';' (sapi.dll's own result); decimal
 *     references are left alone. */

typedef struct xmem {
    struct xmem *next;
    wc data[1];
} xmem;

static wc *xalloc(anna_norm *nm, int n)
{
    xmem *m = calloc(1, sizeof(xmem) + sizeof(wc) * (size_t)n);
    if (!m) return NULL;
    m->next = (xmem *)nm->xmlmem;
    nm->xmlmem = m;
    return m->data;
}

static void sapi_xml_free(anna_norm *nm)
{
    xmem *m = (xmem *)nm->xmlmem;
    while (m) {
        xmem *n = m->next;
        free(m);
        m = n;
    }
    nm->xmlmem = NULL;
}

static int x_isws(wc c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

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
static const wc *phones_parse(wc *s, int n)
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

/* sapi.dll 0-terminates the attribute values it keeps pointers to (<context>, <pron>) in its copy of the text */
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

/* attribute value of a tag (between start and end), or NULL */
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

static int x_name_is(const wc *s, int n, const char *name)
{
    return (int)strlen(name) == n && !wnicmp_a(s, name, n);
}

/* named entities -> character + U+200B padding; hex references "&#x41;" -> U+200B, character, U+200B...,
 * ';' kept (that is what sapi.dll does); decimal references stay as they are */
static void x_entities(wc *s, int n)
{
    static const struct {
        const char *e;
        wc c;
    } E[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
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

static int sapi_xml_parse(anna_norm *nm)
{
    wc *b = nm->buf;
    int n = nm->n, i = 0, sp = 0;
    anna_vstate stack[64], cur = DEFAULT_STATE;
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
                anna_vstate st = cur;
                while (j + 3 <= n && wncmp_a(b + j, "]]>", 3)) j++;
                j = j + 3 <= n ? j + 3 : n;
                while (j < n && x_isws(b[j])) j++;
                st.action = 6;
                if (add_frag(nm, &st, s, j - s, s) < 0) return -1;
                i = j;
                continue;
            }
            te = i + 1;
            while (te < n && b[te] != '>') te++;
            ts = i + 1;
            if (ts < te && b[ts] == '/') {
                close = 1;
                ts++;
            }
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
                if (x_name_is(nm0, nn, "voice") || x_name_is(nm0, nn, "lang")) nm->group_next = 1;
                if (close) {
                    if (sp > 0) cur = stack[--sp];
                } else {
                    const wc *v;
                    int vl;
                    anna_vstate nst = cur;
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
                        wc *c = xalloc(nm, vl + 1);
                        if (!c) return -1;
                        memcpy(c, v, sizeof(wc) * (size_t)vl);
                        c[vl] = 0;
                        nst.ctx = c;
                    } else if (x_name_is(nm0, nn, "partofsp") && (v = x_attr(b + ns, b + te, "part", &vl)) != NULL) {
                        static const struct {
                            const char *n;
                            int pos;
                        } PS[] = {{"noun", 0x1000}, {"verb", 0x2000}, {"modifier", 0x3000},
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
                    anna_vstate st = cur;
                    long x = (v = x_attr(b + ns, b + te, "msec", &vl)) != NULL ? x_long(v, vl) : 0;
                    st.action = 1;
                    st.sil = x < 0 ? 0 : (int)x;
                    if (add_frag(nm, &st, s, 0, s) < 0) return -1;
                    nm->fr[nm->nfr - 1].t = NULL;
                }
            } else if (x_name_is(nm0, nn, "bookmark")) {
                if (!close) {
                    const wc *v;
                    int vl = 0;
                    anna_vstate st = cur;
                    v = x_attr(b + ns, b + te, "mark", &vl);
                    st.action = 3;
                    if (add_frag(nm, &st, v ? (int)(v - b) : s, vl, s) < 0) return -1;
                }
            } else if (x_name_is(nm0, nn, "pron")) {
                if (close) {
                    if (sp > 0) cur = stack[--sp];
                } else {
                    const wc *v;
                    int vl = 0;
                    anna_vstate st = cur;
                    v = x_attr(b + ns, b + te, "sym", &vl);
                    st.action = 2;
                    st.phones = v ? phones_parse((wc *)v, vl) : NULL;
                    if (selfclose) {
                        if (add_frag(nm, &st, s, 0, s) < 0) return -1;
                        nm->fr[nm->nfr - 1].t = NULL;
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
            if (!known) { /* unknown tag: eAction 6 fragment with the tag text and the whitespace after it */
                anna_vstate st = cur;
                int j = i;
                while (j < n && x_isws(b[j])) j++;
                st.action = 6;
                if (add_frag(nm, &st, s, j - s, s) < 0) return -1;
                i = j;
            } else {
                while (i < n && x_isws(b[i])) i++;
            }
            continue;
        }
        { /* text up to the next tag */
            int j = i, k, end;
            while (j < n && b[j] != '<') j++;
            x_entities(b + i, j - i);
            k = i;
            while (k < j && x_isws(b[k])) k++;
            end = j;
            if (j == n)
                while (end > k && x_isws(b[end - 1])) end--;
            if (k < end && add_frag(nm, &cur, k, end - k, k) < 0) return -1;
            i = j;
        }
    }
    return 0;
}
