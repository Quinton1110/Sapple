// SAPI 4 tag lexer and parser of msttssyn.dll, decompiled.
#include "tags.h"
#include "crt_vc.h"
#include "vcrt.h"
#include <string.h>

LAYOUT(TagKeyword, handler, 0x0c);
LAYOUT(TagOut, text_bytes, 0x10);

#define CUR(pp) GP(const char, *(pp))
#define ADV(pp) GPSET(*(pp), CUR(pp) + 1)
// the table's fields one by one (a portable build's pointers are wider than the table's)
#define KW_BASE 0x6371f218
#define KW_NAME(i) GP(const char, DLLPTR(const char, KW_BASE + 16u * (uint32_t)(i))[0])
#define KW_TOKEN(i) (*DLLVAR(const int32_t, KW_BASE + 16u * (uint32_t)(i) + 4))
#define KW_CODE(i) (*DLLVAR(const int32_t, KW_BASE + 16u * (uint32_t)(i) + 8))
#define KW_HANDLER(i) (*DLLVAR(const uint32_t, KW_BASE + 16u * (uint32_t)(i) + 12))
#define NKEYWORDS 22

int tag_lex_number(GPTR(const char) *pp, int32_t *out) {
    if (!*pp || !*CUR(pp) || !out) return TOK_END;
    *out = 0;
    while ((signed char)*CUR(pp) >= '0' && (signed char)*CUR(pp) <= '9') {
        *out = (int32_t)((uint32_t)*out * 10u + (uint32_t)((signed char)*CUR(pp) - '0'));
        ADV(pp);
    }
    return TOK_NUMBER;
}

int tag_keyword(const char *word) {
    for (int i = 0; i < NKEYWORDS; i++)
        if (vc_lstrcmpiA(word, KW_NAME(i)) == 0) return KW_TOKEN(i);
    return TOK_WORD;
}

int tag_lex_word(GPTR(const char) *pp, char *buf, int n) {
    char *o = buf;
    int last = n - 1;
    if (last < 0) { *o = 0; return TOK_END; }
    for (int i = 0; i <= last; i++) {
        signed char ch = (signed char)*CUR(pp);
        if (!ch) return TOK_END;
        if (ch == ',' || (ch > '9' && (ch <= ';' || ch == '=' || ch == '\\'))) {
            *o = 0;
            return tag_keyword(buf);
        }
        *o++ = (char)ch;
        ADV(pp);
    }
    *o = 0;
    return TOK_END;
}

int tag_lex_string(GPTR(const char) *pp, char *buf, int n) {
    buf[0] = *CUR(pp);
    ADV(pp);
    char *o = buf + 1;
    int last = n - 1;
    for (int count = 0; count < last; count++) {
        const char *c = CUR(pp);
        char ch = *c;
        if (!ch) return TOK_END;
        if (ch == '"') {
            *o++ = *CUR(pp);
            ADV(pp);
            *o = 0;
            return TOK_STRING;
        }
        if (ch == '\\') {
            c++;
            if (*c != '\\') { buf[0] = 0; return TOK_END; }
            GPSET(*pp, c);
        }
        *o++ = *c;
        ADV(pp);
    }
    *o = 0;
    return TOK_END;
}

int tag_lex_brace(GPTR(const char) *pp, char *buf, int n) {
    char *o = buf;
    int left = n - 1;
    if (left <= 0) { *o = 0; return TOK_END; }
    for (;;) {
        char ch = *CUR(pp);
        if (!ch || ch == '\\') return TOK_END;
        if (ch == '}') { o[0] = '}'; o[1] = 0; return TOK_BRACE; }
        *o++ = ch;
        ADV(pp);
        if (--left <= 0) break;
    }
    *o = 0;
    return TOK_END;
}

int tag_lex(GPTR(const char) *pp, char *buf, int n) {
    signed char ch = (signed char)*CUR(pp);
    if (ch >= '0' && ch <= '9') return tag_lex_number(pp, (int32_t *)(void *)buf);
    int tok;
    switch (ch) {
    case ',': tok = TOK_COMMA; break;
    case '"': return tag_lex_string(pp, buf, n);
    case ':': tok = TOK_COLON; break;
    case ';': tok = TOK_SEMI; break;
    case '<': tok = TOK_LT; break;
    case '=': tok = TOK_EQ; break;
    case '\\': tok = TOK_BACKSLASH; break;
    case '{': return tag_lex_brace(pp, buf, n);
    default: return tag_lex_word(pp, buf, n);
    }
    ADV(pp);
    return tok;
}

#define E_FAIL ((int32_t)0x80004005)
#define E_OUTOFMEMORY ((int32_t)0x8007000e)

int32_t tag_parse_unsupported(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n) {
    (void)kw; (void)pp; (void)out; (void)buf; (void)n;
    return E_FAIL;
}

int32_t tag_parse_flag(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n) {
    for (;;) {
        int t = tag_lex(pp, buf, n);
        if (t == TOK_END) return E_FAIL;
        if (t == TOK_BACKSLASH) break;
    }
    out->code = KW_CODE(kw);
    return 0;
}

int32_t tag_parse_number(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n) {
    if (tag_lex(pp, buf, n) != TOK_EQ || tag_lex(pp, buf, n) != TOK_NUMBER || tag_lex(pp, buf, n) != TOK_BACKSLASH)
        return E_FAIL;
    out->code = KW_CODE(kw);
    memcpy(&out->value, buf, 4);
    return 0;
}

int32_t tag_parse_string(int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n) {
    if (tag_lex(pp, buf, n) != TOK_EQ || tag_lex(pp, buf, n) != TOK_STRING || tag_lex(pp, buf, n) != TOK_BACKSLASH)
        return E_FAIL;
    char *t = vc_malloc(strlen(buf) + 1);
    GPSET(out->text, t);
    if (!t) return E_OUTOFMEMORY;
    out->code = KW_CODE(kw);
    strcpy(t, buf);
    return 0;
}

static int32_t call_handler(uint32_t h, int kw, GPTR(const char) *pp, TagOut *out, char *buf, int n) {
    switch (h) {
    case 0x6368119e: return tag_parse_flag(kw, pp, out, buf, n);
    case 0x636811d9: return tag_parse_number(kw, pp, out, buf, n);
    case 0x63681237: return tag_parse_string(kw, pp, out, buf, n);
    case 0x63681196: return tag_parse_unsupported(kw, pp, out, buf, n);
    default:
        // only reachable with tok == TOK_EQ ("\\=..."): the original reads a 23rd table entry whose
        // "handler" is the value 1 and faults; this returns E_FAIL instead
#ifdef DECOMP_HOOK
        decomp_warn("tag_parse: tag starting with '=' (the original crashes here)");
#endif
        return E_FAIL;
    }
}

int tag_parse(char *tag, TagOut *out) {
    uint16_t wide[0x400];
    int wn = (int)strlen(tag) + 1;
    if (wn <= 0x400) vc_MultiByteToWideChar_cp1252((const uint8_t *)tag, wn, wide);
    else {
        memset(wide, 0, sizeof wide);
#ifdef DECOMP_HOOK
        decomp_warn("tag_parse: tag longer than 1023 characters (the original copies stale stack)");
#endif
    }
    vc_CharLowerA_str(tag);
    uint32_t len = (uint32_t)strlen(tag);
    char *buf = vc_malloc(len > 4 ? len : 4);
    if (!buf) return 0;
    GPTR(const char) p;
    GPSET(p, tag + 1);
    int tok = tag_lex(&p, buf, (int)strlen(tag + 1));
    if (tok <= TOK_EQ) {
        int32_t r = call_handler(CODEADDR(KW_HANDLER(tok)), tok, &p, out, buf, (int)strlen(GP(const char, p)));
        if (r >= 0) {
            vc_free(buf);
            return 1;
        }
    }
    out->text_bytes = (int32_t)(len * 2);
    char *t = vc_malloc(len * 2);
    GPSET(out->text, t);
    if (!t) {
        vc_free(buf);
        return 0;
    }
    memcpy(t, wide, len * 2);
    out->code = 0xb;
    vc_free(buf);
    return 1;
}
