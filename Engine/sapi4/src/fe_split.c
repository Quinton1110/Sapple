#include "fe_split.h"
#include "vcrt.h"
#include <string.h>

LAYOUT(SplitLexer, saved, 0x24);
LAYOUT(SplitLexer, has_saved, 0x48);
LAYOUT(Brackets, pos, 0xd0);
LAYOUT(Quotes, marks_pos, 0x1c);
LAYOUT(SplitText, base, 8);
LAYOUT(Sentence, flag, 0x20);

#define CLASS(c) DLLVAR(const int32_t, LEX_CLASS)[(uint8_t)(c)]

// ------------------------------------------------------------------ quotes

void quotes_track(Quotes *q, const TextSpan *t, int32_t open, int32_t possessive, int32_t in_word) {
    int32_t k = CLASS(span_first(t));
    if (k == 0xd || k == 0xc) {
        if (CLASS(span_first(t)) == 0xc) {
            if (q->marks == 0) q->marks_pos = span_pos(t);
            q->marks++;
        } else q->marks--;
        return;
    }
    char c = span_f4(t) == 0xb ? span_first(t) : span_last(t);
    if (c == '"') {
        if (open) {
            if (q->dq == 0) q->dq_pos = span_pos(t);
            q->dq++;
        } else q->dq--;
    } else if (c == '\'') {
        if (possessive) q->possessive++;
        else if (open) {
            if (q->sq == 0) q->sq_pos = span_pos(t);
            if (in_word) q->sq_word++;
            q->sq++;
        } else q->sq--;
    }
}

int quotes_open(const Quotes *q) {
    // (the single-quote check between the two is a stub that always answers no)
    return q->dq > 0 || q->marks > 0;
}

char span_before_last(const TextSpan *s) { return GP(char, s->buf)[s->pos + s->len - 2]; }

int quotes_apostrophe(Quotes *q, const TextSpan *t) {
    if (span_last(t) != '\'') return 0;
    int32_t plural = 0;
    if (span_len(t) > 1) {
        char c = span_before_last(t);
        if (c == 's' || c == 'S') plural = 1;
    }
    quotes_track(q, t, 0, plural, 0);
    return 1;
}

void quotes_clamp(Quotes *q) {
    if (q->dq < 0) q->dq = 0, q->dq_pos = 0;
    if (q->marks < 0) q->marks = 0, q->marks_pos = 0;
    if (q->sq < 0) q->sq = 0, q->possessive = 0, q->sq_word = 0, q->sq_pos = 0;
}

// ------------------------------------------------------------------ brackets

void brackets_classify(const TextSpan *t, int32_t *kind, int32_t *dir) {
    switch ((int8_t)span_first(t)) {
    case '(': *kind = 0; *dir = 1; break;
    case ')': *kind = 0; *dir = -1; break;
    case '[': *kind = 2; *dir = 1; break;
    case ']': *kind = 2; *dir = -1; break;
    case '{': *kind = 1; *dir = 1; break;
    case '}': *kind = 1; *dir = -1; break;
    default: break;                         // (outputs left as they were)
    }
}

void brackets_update(Brackets *b, const TextSpan *t, const TextSpan *prev, char *closer) {
    if (b->overflow) return;
    int32_t kind = 0, dir = 0;              // (uninitialised in the original; callers pass brackets only)
    brackets_classify(t, &kind, &dir);
    if (dir == -1) {
        int32_t top = b->top;
        if (top < 0) {
            if (span_len(prev) == 1 && span_f4(prev) == 3) return;
            *closer = 0;
            return;
        }
        int32_t k = ((const int32_t *)(const void *)b)[2 + top];
        b->top = top - 1;
        if (k == kind) return;
        if (k == 0) *closer = ')';
        else if (k == 1) *closer = '}';
        else if (k == 2) *closer = ']';
        return;
    }
    if (dir != 1) return;
    int32_t old = b->top;
    b->top = old + 1;
    if (old >= 50) { b->overflow = 1; return; }
    // the 51st entry lands past both arrays (kind[50] is pos[0], pos[50] the next dword after the
    // object; the splitter's frame keeps what the original has there)
    int32_t *v = (int32_t *)(void *)b;
    v[2 + b->top] = kind;
    v[0x34 + b->top] = span_pos(t);
}

int brackets_peek(const Brackets *b, int32_t *pos) {
    if (b->overflow || b->top < 0) return 0;
    *pos = ((const int32_t *)(const void *)b)[0x34 + b->top];
    return 1;
}

static void brackets_init(Brackets *b) { b->overflow = 0; b->top = -1; }

// ------------------------------------------------------------------ token checks

TextSpan *span_set_kind(TextSpan *s, int32_t kind) {
    s->f4 = kind;
    if (kind == 0) {
        s->pos = s->len = 0;
        s->f10 = 0;
    } else if (kind <= 2 || kind > 5) s->f10 = 0;
    return s;
}

int span_differs(const TextSpan *a, const TextSpan *b) {
    return !(a->buf == b->buf && a->f4 == b->f4 && a->pos == b->pos && a->len == b->len && a->f10 == b->f10);
}

// a word of three or more characters that is not a roman numeral (the second character not a full stop,
// the first not a digit), or 0
int word_not_roman(const TextSpan *t) {
    uint32_t n = (uint32_t)span_len(t);
    if (span_f4(t) != 3 || n < 2) return 0;
    int32_t st = 1;
    const char *p = span_cur(t);
    for (uint32_t i = 0; i < n; i++, p++) {
        if (st == 1) {
            if (vc_isdigit((uint8_t)*p)) return 0;
            st = 2;
        } else if (st == 2) {
            if (*p == '.') return 0;
            st = 3;
        } else if (st == 3) {
            if (*p == '.') return 0;
        }
    }
    if (st != 3) return 0;
    p = span_cur(t);
    for (uint32_t i = 0; i < n; i++, p++) {
        switch ((int8_t)*p) {
        case 'I': case 'V': case 'X': case 'i': case 'v': case 'x': continue;
        default: return 1;
        }
    }
    return 0;
}

// initials: "A.B." (upper-case letter, full stop, repeated; odd length of 3 or more as the lexer cuts it)
int word_initials(const TextSpan *t) {
    uint32_t n = (uint32_t)span_len(t);
    if (span_f4(t) != 3 || n < 3 || (n & 1) != 1) return 0;
    const char *p = span_cur(t);
    int32_t st = 0;
    for (uint32_t i = 0; i < n; i++, p++) {
        if (st == 0) {
            if (!cp1252_isupper((uint8_t)*p)) return 0;
            st = 1;
        } else if (st == 1) {
            if (*p != '.') return 0;
            st = 0;
        }
    }
    return 1;
}

int word_is_number(TextSpan t) { return span_f4(&t) == 3 && vc_isdigit((uint8_t)span_first(&t)); }

int word_number_name(const char *s, uint32_t n) {
    char buf[0x1f8];
    vc_lstrcpynA(buf, s, n + 1);
    if (vc_CharLowerBuffA(buf, n) != n) return 0;
    return lex_find(buf, n, DLLPTR(char, 0x637336b0), 10) >= 0;
}

int word_equals(const char *ref, const char *s, int32_t n) {
    int32_t len = (int32_t)strlen(ref);
    if (n != len) return 0;
    return vc_word_cmp((const uint8_t *)ref, len, (const uint8_t *)s, len, 1) == 0;
}

// ------------------------------------------------------------------ the splitter

#define STR_no 0x63733810       // "no."
#define STR_Dr 0x6373380c       // "Dr."
#define STR_St 0x63733808       // "St."
#define STR_PS 0x63733804       // "PS."
#define STR_P_S 0x637337fc      // "P.S."
#define STR_No 0x637337f8       // "No."
#define STR_at 0x637337f4       // "at"
#define STR_fig 0x637337ec      // "fig."
#define STR_figs 0x637337e4     // "figs."
#define STR_in 0x637337e0       // "in."
#define STR_etc 0x637337d8      // "etc."
#define IS(str, t) word_equals(DLLVAR(const char, str), span_cur(t), span_len(t))

static void split_lexer_accept(SplitLexer *L) {
    L->lx.start = L->lx.cur;
    span_clear(&L->lx.pending);
}

int32_t split_sentences(int32_t unused, SplitText *in, int32_t max, Sentence *out, int32_t *count,
                        int32_t *eot) {
    (void)unused;
    // (in this order, as in the original's frame: a 51st open bracket writes its position over the
    // first word of the saved quote state)
    struct { Brackets br; Quotes q_saved; } f;
#define br f.br
#define q_saved f.q_saved
    Brackets br_saved;
    Quotes q;
    SplitLexer L;
    TextSpan cur, cur_saved, tok, prev, prev_saved, prev2, prev2_saved, nonspace, nonspace_saved;
    brackets_init(&br);
    brackets_init(&br_saved);
    memset(&q, 0, sizeof q);
    memset(&q_saved, 0, sizeof q_saved);
    memset(&L, 0, sizeof L);
    span_clear(&cur);
    span_clear(&cur_saved);
    span_clear(&tok);
    span_clear(&prev);
    span_clear(&prev_saved);
    span_clear(&prev2);
    span_clear(&prev2_saved);
    span_clear(&nonspace);
    span_clear(&nonspace_saved);
    *count = 0;
    *eot = 0;
    lex_init(&L.lx, in->base + in->start, in->total - in->start);
    char *base = GP(char, in->base);
    int32_t sent_start = 0;             // [ebp-0x70]
    int32_t kind = 0;                   // [ebp-0x3c]
    int32_t sv_word_chars = 0, sv_f10 = 0, sv_words = 0, sv_chars = 0, sv_kind = 0;

    for (;;) {                          // one sentence
        int32_t state = 0;
        int32_t chars = 0, words = 0, word_chars = 0, word_f10 = 0, mark = 0, flag = 0, bracket_start = 0,
                after_at = 0, quote_pos = 0, same_end = 0, after_semi = 0, semicolon = 0;
        *DLLVAR(int32_t, 0x63739990) = 0;
        *DLLVAR(int32_t, 0x637399d4) = 0;

        do {
            int32_t prev_kind = span_f4(&prev);     // [ebp-0x38]
            if (prev_kind != 0xb) nonspace = prev;
            lex_next(&L.lx, &tok);
            if (span_differs(&tok, &cur)) {
                cur = tok;
                kind = span_f4(&cur);
                int32_t len = span_len(&cur);
                if (kind >= 3) {
                    if (kind <= 5) {
                        word_chars += len;
                        chars += len;
                        word_f10 += span_f10(&cur);
                        words++;
                        if (semicolon) {
                            after_semi++;
                            semicolon = 0;
                        }
                    } else if (kind == 0xa || kind == 0x10) semicolon = 0;
                    else if (kind == 0xc) {
                        if (span_first(&cur) == ';') semicolon = 1;
                    }
                }
                if (!(kind == 1 || kind == 2 || kind == 0xe || kind == 3 || kind == 4 || kind == 5))
                    chars += span_len(&cur);
            }

#define ACCEPT() do { split_lexer_accept(&L); prev2 = prev; prev = cur; } while (0)
#define BRACKET() do { char cl_; brackets_update(&br, &cur, &prev, &cl_); } while (0)
#define GO(s) do { state = (s); goto next; } while (0)
#define TAKE(s) do { state = (s); goto take; } while (0)
            switch (state) {
            case 0:
                if (kind <= 0) GO(2);
                if (kind <= 2) goto take;
                if (kind == 8) {
                    BRACKET();
                    TAKE(1);
                }
                if (kind == 0xb) {
                    quotes_track(&q, &cur, 1, 0, 0);
                    goto take;
                }
                GO(2);
            case 1:
                if (kind <= 0) GO(2);
                if (kind <= 2) goto take;
                if (kind <= 7) GO(2);
                if (kind <= 9) goto bracket_take;
                GO(2);
            case 2:
                if (kind <= 0) GO(7);
                if (kind <= 2) goto take;
                if (kind != 3) GO(7);
                if (word_not_roman(&cur)) GO(7);
                // a possible roman numeral: try ahead from here, keeping everything to come back to
                L.saved = L.lx;
                L.has_saved = 1;
                sv_word_chars = word_chars;
                sv_f10 = word_f10;
                sv_words = words;
                sv_chars = chars;
                sv_kind = 3;
                cur_saved = cur;
                prev_saved = prev;
                nonspace_saved = nonspace;
                prev2_saved = prev2;
                br_saved = br;
                q_saved = q;
                ACCEPT();
                GO(3);
            case 3:
                if (kind <= 0) GO(5);
                if (kind <= 2) {
                    ACCEPT();
                    GO(6);
                }
                if (kind == 9) {
                    BRACKET();
                    TAKE(4);
                }
                if (kind == 0xa && span_first(&cur) == '.') TAKE(4);
                GO(5);
            case 4:
                if (kind <= 0) GO(5);
                if (kind <= 2) {
                    mark = span_pos(&cur);
                    chars = words = word_chars = word_f10 = 0;
                    TAKE(7);
                }
                if (kind == 9) goto bracket_take;
                if (kind == 0xa && span_first(&cur) == '.') goto take;
                GO(5);
            case 5:
                L.lx = L.saved;
                word_chars = sv_word_chars;
                word_f10 = sv_f10;
                words = sv_words;
                chars = sv_chars;
                kind = sv_kind;
                cur = cur_saved;
                prev = prev_saved;
                nonspace = nonspace_saved;
                prev2 = prev2_saved;
                br = br_saved;
                q = q_saved;
                GO(7);
            case 6:
                if (kind == 9) {
                    BRACKET();
                    TAKE(4);
                }
                GO(5);
            case 7:
                if (kind > 9) {
                    if (kind == 0xa) {
                        flag = 1;
                        ACCEPT();
                        GO(0xe);
                    }
                    if (kind == 0xb) TAKE(0xa);
                    if (kind <= 0xd) goto take;
                    if (kind <= 0xf) {
                        *eot = 1;
                        GO(0x17);
                    }
                    if (kind == 0x10) TAKE(0x11);
                    goto take;
                }
                if (kind == 9) {
                    int32_t open;
                    if (brackets_peek(&br, &open) && open >= mark) {
                        bracket_start = 1;
                        state = 0x16;
                        const char *p = base + in->start + sent_start;
                        int32_t n = open--;
                        if (n > mark) {
                            for (;;) {
                                char c = *p++;
                                if (CLASS(c) != 7) {
                                    bracket_start = 0;
                                    state = 7;
                                    break;
                                }
                                n = open--;
                                if (!(n > mark)) break;
                            }
                        }
                    }
                    goto bracket_take;
                }
                if (kind <= 0) goto take;
                if (kind <= 2) TAKE(0x12);
                if (kind <= 5) {
                    flag = 0;
                    bracket_start = 0;
                    if (quotes_apostrophe(&q, &cur)) quotes_clamp(&q);
                    if (kind == 3) {
                        ACCEPT();
                        if (span_len(&cur) == 1) {
                            if (vc_isdigit((uint8_t)span_first(&cur))) goto next;
                            GO(0xb);
                        }
                        if ((span_len(&cur) & 1) != 1) goto next;
                        GO(8);
                    }
                    if (kind == 4) {
                        ACCEPT();
                        if (words == 1 && (IS(STR_PS, &prev) || IS(STR_P_S, &prev))) goto next;
                        GO(0xc);
                    }
                    // kind 5
                    if ((IS(STR_Dr, &cur) || IS(STR_St, &cur)) && span_f4(&prev2) != 0 &&
                        cp1252_isupper((uint8_t)span_first(&prev2))) {
                        span_set_kind(&cur, 4);
                        TAKE(0xc);
                    }
                    goto take;
                }
                if (kind <= 7) TAKE(0x11);
                if (kind == 8) goto bracket_take;
                goto take;
            case 8:
                if (kind == 0xa && span_first(&cur) == '.' && word_initials(&prev)) TAKE(9);
                GO(7);
            case 9:
                if (kind >= 0xe && kind <= 0xf) {
                    flag = 1;
                    *eot = 1;
                    GO(0x17);
                }
                GO(7);
            case 10: {
                int32_t num = 0, apos = 0, k2 = span_f4(&prev2), open;
                if (kind == 3) num = word_is_number(cur);
                if (k2 == 1 || k2 == 0 || k2 == 8) {
                    quotes_track(&q, &prev, 1, 0, num);
                    quote_pos = span_pos(&cur);
                } else {
                    open = span_pos(&prev) == quote_pos;
                    if (open) quote_pos = span_pos(&cur);
                    else if ((uint32_t)span_len(&prev) > 0 && span_first(&prev) == '\'' &&
                             (uint32_t)span_len(&cur) > 0 && vc_IsCharAlphaNumericA((uint8_t)span_first(&cur)))
                        apos = 1;
                    quotes_track(&q, &prev, open, apos, num);
                }
                quotes_clamp(&q);
                GO(7);
            }
            case 11:
                if (kind == 0xa && span_first(&cur) == '.') TAKE(0xc);
                GO(7);
            case 12:
                if (kind > 9) {
                    if (kind == 0xa) goto flag_19;
                    if (kind == 0xb) {
                        if (span_first(&cur) == '"' || CLASS(span_first(&cur)) == 0xd) goto flag_19;
                        GO(7);
                    }
                    if (kind <= 0xd) GO(7);
                    if (kind <= 0xf) {
                        *eot = 1;
                        if ((prev_kind == 4 || prev_kind == 5) && span_f4(&prev2) == 0 && !IS(STR_No, &prev))
                            GO(0x17);
                        goto flag_end;
                    }
                    if (kind == 0x10) goto flag_19;
                    GO(7);
                }
                if (kind == 9) {
                    if (span_f4(&prev2) == 8) GO(7);
                    goto flag_19;
                }
                if (kind <= 0) GO(7);
                if (kind <= 2) {
                    if ((uint32_t)span_len(&cur) > 1) goto flag_19;
                    TAKE(0xd);
                }
                GO(7);
            case 13:
                if (kind < 3) GO(7);
                if (kind <= 5) {
                    if (IS(STR_no, &prev2) || IS(STR_fig, &prev2) || IS(STR_figs, &prev2)) {
                        if (word_is_number(cur)) GO(7);
                        if (word_number_name(span_cur(&cur), (uint32_t)span_len(&cur))) GO(7);
                        goto flag_end;
                    }
                    if (!cp1252_isupper((uint8_t)span_first(&cur))) GO(7);
                    if (span_f4(&prev2) != 4 && span_f4(&prev2) != 0xa) GO(7);
                    int32_t e = span_pos(&cur) + span_len(&cur) + in->start;
                    if (IS(STR_in, &prev2) || IS(STR_etc, &prev2)) goto flag_end;
                    if (!lex_common(span_cur(&cur), (uint32_t)span_len(&cur))) GO(7);
                    if (e >= in->total) GO(7);
                    if (base[e] == '.') GO(7);
                    goto flag_end;
                }
                if (kind <= 8) GO(7);
                if (kind <= 10) goto flag_19;
                GO(7);
            case 14:
                flag = 1;
                if (kind <= 0) GO(7);
                if (kind <= 2) GO(0x13);
                if (kind == 9) {
                    int32_t open;
                    state = 0x13;
                    bracket_start = 1;
                    if (!brackets_peek(&br, &open)) goto next;
                    const char *p = base + in->start + sent_start;
                    for (;;) {
                        int32_t n = open--;
                        if (n <= sent_start) GO(0x13);
                        char c = *p++;
                        if (!vc_IsCharAlphaNumericA((uint8_t)c)) continue;
                        ACCEPT();
                        bracket_start = 0;
                        GO(0x13);
                    }
                }
                if (kind == 0xa || kind == 0x10) GO(0x13);
                if (kind == 0xb) {
                    if (span_first(&cur) == '\'') {
                        quotes_track(&q, &cur, 0, 0, 0);
                        quotes_clamp(&q);
                        ACCEPT();
                    }
                    GO(0x13);
                }
                GO(7);
            case 15:
                if (kind < 3) GO(7);
                if (kind <= 5) {
                    if (cp1252_isupper((uint8_t)span_first(&cur))) goto flag_end;
                    GO(7);
                }
                if (kind <= 7) GO(7);
                if (kind <= 10) goto flag_19;
                if (kind == 0xb) GO(0x17);
                GO(7);
            case 16:
                goto next;
            case 17:
                flag = 1;
                if (kind <= 0 || kind > 2) GO(7);
                TAKE(0xf);
            case 18:
                if (kind < 6 || kind > 7) GO(7);
                TAKE(7);
            case 19:
            case 20:
            case 21: {
                int32_t st = state;
                if (st == 19) st = quotes_open(&q) ? 0x15 : 0x14;
                state = st;
                if (kind > 9) {
                    if (kind == 0xa) goto take;
                    if (kind == 0xb) {
                        quotes_track(&q, &cur, 0, 0, 0);
                        quotes_clamp(&q);
                        ACCEPT();
                        if (span_first(&cur) == '"') state = st == 0x14 ? 0x15 : 0x13;
                        goto next;
                    }
                    if (kind == 0xc) GO(7);
                    if (kind <= 0xd) GO(0x17);
                    if (kind <= 0xf) {
                        *eot = 1;
                        GO(0x17);
                    }
                    if (kind == 0x10) goto take;
                    GO(0x17);
                }
                if (kind == 9) {
                    int32_t open;
                    int ok = brackets_peek(&br, &open);
                    BRACKET();
                    if (ok && open != sent_start) goto take;
                    bracket_start = 1;
                    goto take;
                }
                if (kind == 2) goto take;
                if (kind != 1) GO(0x17);
                if (!bracket_start && span_len(&cur) == 1 && prev_kind == 0xb) TAKE(0x16);
                if (st != 0x14) goto take;
                if (!bracket_start && span_len(&cur) == 1) {
                    int32_t open;
                    if (prev_kind == 0xb || prev_kind == 9) TAKE(0x16);
                    if (brackets_peek(&br, &open)) TAKE(0x16);
                }
                if (span_f4(&prev2) == 3 && prev_kind == 0xa && span_len(&prev) == 1 &&
                    span_first(&prev) == '.' && (after_at = IS(STR_at, &prev2)) != 0)
                    TAKE(0x16);
                TAKE(0x17);
            }
            case 22:
                if (kind <= 0) state = 7;
                else if (kind <= 2) ACCEPT();
                else if (kind <= 5) {
                    if (after_at) state = IS(STR_no, &cur) ? 7 : 0x17;
                    else state = cp1252_isupper((uint8_t)span_first(&cur)) ? 0x17 : 7;
                } else if (kind == 8) state = 0x17;
                else if (kind == 9) state = 0x13;
                else if (kind == 0xb) state = prev_kind - 1 != 0 ? 7 : 0x17;
                else state = 7;
                after_at = 0;
                goto next;
            default:
                goto next;
            }
        flag_19:
            flag = 1;
            GO(0x13);
        flag_end:
            flag = 1;
            GO(0x17);
        bracket_take:
            BRACKET();
        take:
            ACCEPT();
        next:;
        } while (state != 0x17);

        // the end of the sentence: look at the token after it
        int32_t end = span_pos(&cur);
        (void)span_f4(&prev);
        lex_next(&L.lx, &cur);
        kind = span_f4(&cur);
        if (kind == 0xe) {
            *eot = 1;
            ACCEPT();
            lex_next(&L.lx, &cur);
        } else if (span_pos(&cur) == end) same_end = 1;
        int32_t raw = span_pos(&cur) - sent_start;
        uint32_t len = (uint32_t)raw;
        if (raw != 0) {
            const char *p = base + raw + in->start - 1;
            for (;;) {
                int32_t c = (int8_t)*p;
                if (!(c == ' ' || c == *DLLVAR(uint8_t, 0x637399e0) || c == *DLLVAR(uint8_t, 0x637399e1))) break;
                len--;
                p--;
                if (!(len > 0)) break;
            }
        }
        if (same_end) {
            if (kind <= 0) chars -= span_len(&cur);
            else if (kind <= 2) {
            } else if (kind <= 5) {
                word_chars -= span_len(&cur);
                chars -= span_len(&cur);
                word_f10 -= span_f10(&cur);
                words--;
            } else if (kind == 0xe) {
            } else chars -= span_len(&cur);
        }
        Sentence *s = &out[*count];
        if (raw == 0) {
            s->start = in->start;
            s->len = s->raw_len = s->chars = s->words = s->word_chars = s->word_f10 = s->after_semicolon =
                s->flag = 0;
            return 0;
        }
        if (*count >= max) return 5;
        s->start = sent_start + in->start;
        s->len = (int32_t)len;
        s->raw_len = raw;
        s->chars = chars;
        s->words = words;
        s->word_chars = word_chars;
        s->word_f10 = word_f10;
        s->after_semicolon = after_semi;
        s->flag = flag;
        (*count)++;
        if (*count == max) return 0;
        sent_start = span_pos(&cur);
    }
}
#undef br
#undef q_saved

// ------------------------------------------------------------------ the text input

#include "crt_vc.h"
#include "fe_text.h"

char *text_sentences(char *text, int32_t spell) {
    GPTR(char) rewritten = 0;
    SplitText in;
    Sentence s;
    int32_t count = 0, eot = 0;
    char *src = text;
    txt_rewrite(text, &rewritten, spell);
    if (rewritten) {
        src = GP(char, rewritten);
        in.total = (int32_t)strlen(src);
        in.start = 0;
    }
    if (!rewritten) {
        src = text;
        in.total = (int32_t)strlen(text);
        in.start = 0;
    }
    GPSET(in.base, src);
    char *buf = vc_malloc((size_t)(uint32_t)((in.total + 1) * 3)), *o = buf;
    while (in.start < in.total) {
        split_sentences(0, &in, 1, &s, &count, &eot);
        memcpy(o, src + s.start, (size_t)(uint32_t)s.raw_len);
        for (int32_t i = 0; i < s.raw_len; i++, o++)
            if (*o == '\n' || *o == '\r') *o = ' ';
        memcpy(o, DLLVAR(const char, 0x6371efe0), 3);     // " \r\n"
        o += 3;
        in.start += s.raw_len;
    }
    *o = 0;
    if (rewritten) vc_free(GP(char, rewritten));
    return buf;
}

char *text_prepare(char *text, int32_t spell) {
    char *p = text;
    if (*text) {
        do {
            if (*p == '\n') {
                *p = '\r';
                if (p[1] == '\r') *++p = '\n';
            } else if (*p == '\r') {
                if (p[1] == '\n') p++;
            }
            p++;
        } while (*p);
    }
    char *pending = NULL;               // a line break that becomes a space if text follows it
    int blank = 0;
    p = text;
    if (*text) {
        do {
            switch (*p) {
            case '\t':
            case ' ': break;
            case '\n': *p = ' '; break;
            case '\r':
                if (blank) {
                    *p = ' ';
                    if (pending) pending = NULL;
                } else {
                    blank = 1;
                    pending = p;
                }
                break;
            case 0x1b:
                // to the next separator (with none the original reads address 1 and faults; this stops)
                p = strpbrk(p, DLLVAR(const char, 0x6371efd8));   // " \t\r\n\x1a"
                if (!p) p = text + strlen(text) - 1;
                break;
            default:
                if (pending) {
                    *pending = ' ';
                    pending = NULL;
                }
                blank = 0;
                break;
            }
            p++;
        } while (*p);
    }
    return text_sentences(text, spell);
}
