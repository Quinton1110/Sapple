/*
 * TextIn: the text front end.
 *
 * Reads raw characters from the engine's input ring, splits them into
 * tokens (words, numbers, punctuation, embedded ESC sequences), classifies
 * each token into a 96-bit class set, expands tokens that need it, and
 * passes the resulting text on to the preformatter.  Up to 210 tokens are
 * kept in a doubly linked list; the last ~100 already-expanded tokens stay
 * as context for the expansion rules.
 */
#include "engine.h"
#include "crt.h"

/* Character classes used by the tokenizer (indexed by 0..255):
 *   0x01 whitespace       0x02 trailing punctuation   0x04 letter
 *   0x08 digit            0x10 operator               0x20 symbol
 *   0x40 special (newline, ESC, @, \, cp1252 symbols) */
/* @0x100ee810 */
extern const uint32_t g_tok_class[256];

/* Character classes used by the classifier:
 *   0x01 newline  0x02 punctuation  0x04 letter  0x08 digit  0x10 upper case
 *   0x20 quote/bracket  0x40 '+'  0x60 other operators  0x80 vowel
 * Note the original indexes this with a sign-extended char in places, so
 * bytes >= 0x80 read the 128 entries preceding the table (see cls2_signed). */
/* @0x100ef6e0 */
extern const uint32_t g_cls2[256];

/* "\x1b[" -- the start of an embedded control sequence. */
/* @0x100eec20 */
extern const char g_esc_prefix[3];

/* "-$" */
/* @0x100eec1c */
extern const char g_minus_dollar[];

/* g_cls2[(signed char)c]: for c >= 0x80 this reaches the dwords stored in
 * front of the table, which are there in both builds. */
static uint32_t cls2_signed(uint8_t c)
{
    return g_cls2[(int8_t)c];
}

/* ---- bit sets ---------------------------------------------------------- */

/* @0x1001aca0 */
int32_t TV_CDECL Bits_Test(int32_t bit, const uint32_t *bits)
{
    uint32_t mask = 1u << (bit & 31);
    if (bits == NULL)
        return 0;
    return (bits[2 - bit / 32] & mask) != 0;
}

/* @0x1001ace0 */
uint32_t TV_CDECL Bits_Set(int32_t bit, uint32_t *bits)
{
    uint32_t mask = 1u << (bit & 31);
    bits[2 - bit / 32] |= mask;
    return mask;
}

/* Lowest set class above `bit`, or 0 if none. */
/* @0x1001ad10 */
int32_t TV_CDECL Bits_Next(int32_t bit, const uint32_t *bits)
{
    int32_t k = bit + 1;
    int32_t w = 2 - k / 32;
    int32_t b = k & 31;
    uint32_t m = 1u << b;
    const uint32_t *p = &bits[w];

    while (w >= 0) {
        if (b < 32) {
            uint32_t v = *p;
            do {
                if (v & m)
                    return (2 - w) * 32 + b;
                m <<= 1;
                b++;
            } while (b < 32);
        }
        m = 1;
        b = 0;
        p--;
        w--;
    }
    return 0;
}

/* (Re)allocate *p to hold n characters plus one. */
/* @0x1001ad80 */
int32_t TV_CDECL AllocString(char **p, int32_t n)
{
    char *s;
    if (*p != NULL)
        tv_free(*p);
    *p = NULL;
    if (n == 0)
        return 1;
    s = (char *)tv_malloc((size_t)(n + 1));
    if (s == NULL)
        return -1;
    *p = s;
    return 1;
}

/* ---- token list -------------------------------------------------------- */

static void token_init(Token *t)
{
    t->w08 = 0;
    t->w0a = 0;
    t->bits[0] = 0;
    t->bits[1] = 0;
    t->bits[2] = 0;
    t->d18 = NULL;
    t->d1c = 0;
    t->text = NULL;
    t->len = 0;
    t->text2 = NULL;
    t->types = NULL;
    t->trail = 0;
    t->is_number = 0;
    t->num = 0;
    t->w34 = 1;
}

/* @0x1001aa80 */
Token *TV_THISCALL TextIn_InsertAfter(TextIn *self, Token *ref)
{
    Token *t = (Token *)tv_malloc(sizeof(Token));
    t->prev = ref;
    if (ref != NULL) {
        t->next = ref->next;
        ref->next = t;
        if (t->next != NULL)
            t->next->prev = t;
    } else {
        t->next = NULL;
        self->head = t;
    }
    if (self->tail == ref)
        self->tail = t;
    token_init(t);
    self->count++;
    return t;
}

/* @0x1001ab00 */
Token *TV_THISCALL TextIn_InsertBefore(TextIn *self, Token *ref)
{
    Token *t = (Token *)tv_malloc(sizeof(Token));
    t->next = ref;
    t->prev = NULL;
    if (ref != NULL) {
        t->prev = ref->prev;
        ref->prev = t;
        if (t->prev != NULL)
            t->prev->next = t;
        else
            self->head = t;
    } else {
        self->head = t;
        self->tail = t;
    }
    token_init(t);
    self->count++;
    return t;
}

/* Insert a token whose replacement text is `s` (before `ref` when dir is
 * -1, after it otherwise). */
/* @0x1001ab80 */
int32_t TV_THISCALL TextIn_InsertText(TextIn *self, Token *ref, const char *s, int32_t dir)
{
    Token *t = dir == -1 ? TextIn_InsertBefore(self, ref) : TextIn_InsertAfter(self, ref);
    if (AllocString(&t->text2, (int32_t)strlen(s) + 1) == -1)
        return TextIn_Error(self, 0);
    strcpy(t->text2, s);
    t->trail = ' ';
    return 1;
}

/* Unlink and free a token; returns its predecessor (dir -1) or successor. */
/* @0x1001ac10 */
Token *TV_THISCALL TextIn_RemoveToken(TextIn *self, Token *t, int32_t dir)
{
    Token *ret;
    if (t == NULL)
        return NULL;
    ret = dir == -1 ? t->prev : t->next;
    if (t->prev != NULL)
        t->prev->next = t->next;
    else
        self->head = t->next;
    if (t->next != NULL)
        t->next->prev = t->prev;
    if (t->text != NULL)
        tv_free(t->text);
    if (t->text2 != NULL)
        tv_free(t->text2);
    if (t->types != NULL)
        tv_free(t->types);
    tv_free(t);
    self->count--;
    return ret;
}

/* ---- input and output -------------------------------------------------- */

/* Next raw character; -2 when the input ring is empty. */
/* @0x1001add0 */
int32_t TV_THISCALL TextIn_GetChar(TextIn *self)
{
    int32_t c;
    if (self->engine == NULL)
        return -1;
    c = Engine_InGet(self->engine);
    if (c == -1) {
        self->engine->st_input_empty = 1;
        self->input_done = 1;
        return -2;
    }
    return c;
}

/* @0x1001ae10 */
int32_t TV_THISCALL TextIn_Unget(TextIn *self)
{
    if (self->engine == NULL)
        return -1;
    return Engine_InUnget(self->engine);
}

/* @0x1001ae30 */
int32_t TV_THISCALL TextIn_PutString(TextIn *self, const char *s)
{
    int32_t n = (int32_t)strlen(s);
    int32_t i;
    for (i = 0; i < n; i++)
        Preformat_PutChar(self->engine, (uint8_t)s[i]);
    if (i > 0)
        self->engine->st_idle = 0;
    return 0;
}

/* Record an error code (at most 10 are kept); returns -1. */
/* @0x1001ae80 */
int32_t TV_THISCALL TextIn_Error(TextIn *self, int32_t code)
{
    if (self->nerr < 10) {
        self->err[self->nerr] = code;
        self->nerr++;
    }
    return -1;
}

/* ---- lifecycle ----------------------------------------------------------- */

/* @0x10029b10 */
TextIn *TV_THISCALL TextIn_Construct(TextIn *self, int32_t mode)
{
    self->d18 = 0;
    self->head = NULL;
    self->in_quote = 0;
    self->cur = NULL;
    self->tail = NULL;
    self->count = 0;
    self->quote_prefix[0] = 0;
    self->nerr = 0;
    self->mode = mode;
    self->d04 = 0;
    return self;
}

/* Forget all tokens (they have already been emitted and freed). */
/* @0x10029b40 */
int32_t TV_THISCALL TextIn_Reset(TextIn *self)
{
    self->d18 = 0;
    self->head = NULL;
    self->in_quote = 0;
    self->cur = NULL;
    self->tail = NULL;
    self->count = 0;
    self->quote_prefix[0] = 0;
    self->nerr = 0;
    return 1;
}

/* @0x10055ec0 */
uint8_t TV_THISCALL Engine_CreateTextIn(Engine *self)
{
    TextIn *ti = (TextIn *)tv_new(sizeof(TextIn));
    if (ti != NULL)
        ti = TextIn_Construct(ti, 0);
    if (ti == NULL)
        return 0;
    ti->engine = self;
    self->textin = ti;
    return 1;
}

/* ---- tokenizer ----------------------------------------------------------- */

/* Read one or more embedded "ESC [ ... <letter>" sequences into tokens of
 * their own (classes 0x43, 0x44).  Returns the character following them. */
/* @0x1002a0b0 */
int32_t TV_THISCALL TextIn_ReadEscape(TextIn *self)
{
    char buf[20];
    int32_t c, k;
    Token *t;

    buf[0] = g_esc_prefix[0];
    buf[1] = g_esc_prefix[1];
    buf[2] = g_esc_prefix[2];
    memset(buf + 3, 0, sizeof buf - 3);
    for (;;) {
        k = 2;
        c = TextIn_GetChar(self);
        if (c == -2)
            return -2;
        if (c == 0x1b)
            continue;
        if (c != '[')
            return c;
        c = TextIn_GetChar(self);
        if (c == -2)
            return -2;
        while (!(g_tok_class[c] & 4)) {
            /* The original has no bound here: longer sequences corrupt its
             * stack (no reference output exists for them).  Excess
             * characters are dropped instead. */
            if (k < (int32_t)sizeof buf - 2)
                buf[k++] = (char)c;
            c = TextIn_GetChar(self);
            if (c == -2)
                return -2;
        }
        buf[k++] = (char)c;
        buf[k] = 0;
        t = TextIn_InsertAfter(self, self->tail);
        if (t == NULL)
            return TextIn_Error(self, 1);
        if (AllocString(&t->text, k + 1) == -1)
            return TextIn_Error(self, 0);
        strcpy(t->text, buf);
        t->len = (int16_t)k;
        Bits_Set(0x43, t->bits);
        Bits_Set(0x44, t->bits);
        c = TextIn_GetChar(self);
        if (c == -2)
            return -2;
        if (c != 0x1b)
            return c;
    }
}

static int is_symbol_char(int32_t c)
{
    switch (c) {
    case 0xa2: case 0xa3: case 0xa5: case 0xa7: case 0xa9: case 0xae: case 0xb1:
    case 0xb6: case 0xbc: case 0xbd: case 0xbe: case 0xd7: case 0xf7:
        return 1;
    default:
        return 0;
    }
}

/* Read the next token from the input and append it to the list.
 * Returns 1 with *out set, 0 when the input is exhausted, -1 on error. */
/* @0x10029cb0 */
int32_t TV_THISCALL TextIn_ReadToken(TextIn *self, Token **out)
{
    char text[104];
    uint8_t types[104];
    int32_t len = 0;
    int32_t nlead = 0;       /* leading symbol characters */
    int32_t is_number = 0;   /* token started with a digit */
    int32_t is_word = 0;     /* token started with a letter */
    int32_t nsplits = 0;
    uint32_t prevcls = 0, cls = 0;
    int32_t c = 0;
    Token *t;
    int32_t i;

    for (;;) {
        if (len >= 100) {
            c = 0;
            cls = 0;
            break;
        }
        c = TextIn_GetChar(self);
        if (c == -2) {
            c = 0;
            if (len == 0)
                return 0;
            cls = 0;
            break;
        }
        if (c == 0x1b) {
            if (len != 0) {
                cls = g_tok_class[c];
                break;
            }
            c = TextIn_ReadEscape(self);
        }
        if (c == -2)
            return 0;
        if (is_symbol_char(c)) {
            if (len != 0)
                TextIn_Unget(self);
            else
                text[len++] = (char)c;
            c = 0;
            cls = 0;
            break;
        }
        if (c == 0 || c == '\r')
            c = ' ';
        types[len] = 1;
        cls = g_tok_class[c];
        if (c == '\n') {
            if (len != 0)
                break;
            text[len++] = '\n';
            c = 0;
            cls = 0;
            break;
        }
        if (cls & 1)
            break;
        if (c == '.' && is_word) {
            types[len] = 3;
            nsplits++;
        } else if (cls & 0x20) {
            if (is_number || is_word)
                break;
            nlead++;
        }
        if (cls & 8) {
            if (len == 0) {
                is_number = 1;
            } else if (nlead != 0 || (prevcls & 0x20)) {
                break;
            }
            if (prevcls == 4) {
                types[len] = 2;
                nsplits++;
            }
            is_word = 0;
        }
        if (cls & 4) {
            if (len == 0) {
                is_word = 1;
            } else if (nlead != 0) {
                break;
            }
            if (prevcls == 8) {
                types[len] = 2;
                nsplits++;
            }
            is_number = 0;
        }
        text[len++] = (char)c;
        prevcls = cls;
    }

    /* Give trailing punctuation back to the input. */
    if (nlead < len && (g_tok_class[(uint8_t)text[len - 1]] & 2)) {
        do {
            len--;
            c = (uint8_t)text[len];
            text[len] = 0;
            cls = g_tok_class[c];
            TextIn_Unget(self);
        } while (g_tok_class[len > 0 ? (uint8_t)text[len - 1] : 0] & 2);
    }

    text[len] = 0;
    t = TextIn_InsertAfter(self, self->tail);
    if (t == NULL)
        return TextIn_Error(self, 1);
    if (len > 0) {
        if (AllocString(&t->text, len + 1) == -1)
            return TextIn_Error(self, 0);
        strcpy(t->text, text);
    }
    t->len = (int16_t)len;
    if (cls & 0x4c) {
        c = 0;
        TextIn_Unget(self);
    }
    t->trail = (uint8_t)c;
    if (t->text != NULL && tv_strcmp(t->text, g_minus_dollar) == 0 && c == 0) {
        t->text[1] = 0;
        t->trail = '$';
    }
    if (is_number) {
        t->num = tv_atol(text);
        t->w34 = 0;
        t->is_number = 1;
    } else {
        if (nsplits != 0) {
            if (AllocString(&t->types, len + 1) == -1)
                return TextIn_Error(self, 0);
            for (i = 0; i < len; i++)
                t->types[i] = (char)types[i];
            t->types[len] = 1;
        }
        t->w34 = 1;
        t->is_number = 0;
    }
    *out = t;
    return 1;
}

/* Split a token at the marks the tokenizer recorded (letter/digit
 * boundaries, abbreviation dots), then classify every piece. */
/* @0x1002a260 */
int32_t TV_THISCALL TextIn_Split(TextIn *self, Token **pt)
{
    Token *t = *pt;
    Token *nt, *first_new = NULL;
    char seg[104];
    int32_t i, j;

    TextIn_Classify(self, t);
    if (t->w34 == 0)
        return 1;
    if (t->types == NULL) {
        TextIn_Stub(self, t);
        return 1;
    }
    if (Bits_Test(7, t->bits))
        return 1;

    i = 0;
    j = 0;
    if (t->len > 0) {
        do {
            if (t->types[i] != 1) {
                nt = TextIn_InsertBefore(self, t);
                if (nt == NULL)
                    return TextIn_Error(self, 0);
                if (first_new == NULL)
                    first_new = nt;
                if (AllocString(&nt->text, j + 5) == -1)
                    return TextIn_Error(self, 0);
                if (t->types[i] == 2) {
                    nt->trail = 0;
                } else {
                    nt->trail = (uint8_t)t->text[i];
                    i++;
                    t->types[i] = 1;
                }
                seg[j] = 0;
                strcpy(nt->text, seg);
                nt->len = (int16_t)j;
                j = 0;
            }
            seg[j] = t->text[i];
            i++;
            j++;
        } while (t->len > i);
    }
    if (first_new == NULL)
        return 1;
    seg[j] = 0;
    strcpy(t->text, seg);
    t->len = (int16_t)j;
    tv_free(t->types);
    t->types = NULL;
    for (nt = t;; nt = nt->prev) {
        TextIn_Classify(self, nt);
        TextIn_Stub(self, nt);
        if (first_new == nt)
            return 1;
    }
}

/* @0x1002b9e0 */
int32_t TV_THISCALL TextIn_Stub(TextIn *self, Token *t)
{
    (void)self;
    (void)t;
    return 1;
}

/* Derive the class set of a token from its characters. */
/* @0x1002b710 */
int32_t TV_THISCALL TextIn_Classify(TextIn *self, Token *t)
{
    int32_t n_upper = 0, n_letter = 0, n_dotletter = 0, n_digit = 0, n_op = 0;
    int32_t n_vowel = 0, n_punct = 0, n_cr = 0, first_upper = 0, i = 0;
    uint32_t cls, prevcls = 0;
    uint8_t prev = 0, c;
    const char *s;

    t->bits[0] = 0;
    t->bits[1] = 0;
    t->bits[2] = 0;
    if (t->is_number != 0)
        return TextIn_ClassifyNumber(self, t);

    s = t->text;
    if (s == NULL) {
        if (!(cls2_signed(t->trail) & 0x40))
            Bits_Set(0x43, t->bits);
        return 1;
    }
    if (s[0] == '\n' || s[1] == '\n') {
        Bits_Set(0x45, t->bits);
        Bits_Set(0x46, t->bits);
        return 1;
    }
    for (i = 0; s[i] != 0; i++) {
        c = (uint8_t)s[i];
        cls = g_cls2[c];
        if (c == '\r')
            n_cr++;
        if (cls & 8)
            n_digit++;
        if (cls & 0x40)
            n_op++;
        if (cls & 4) {
            n_letter++;
            if (prev == '.')
                n_dotletter++;
        }
        if (cls & 0x10) {
            n_upper++;
            if (i == 0)
                first_upper = 1;
        }
        if (cls & 0x80)
            n_vowel++;
        if (cls & 0x20)
            n_punct++;
        if (c == '.' && (prevcls & 4))
            n_dotletter++;
        prev = c;
        prevcls = cls;
    }

    if (first_upper && n_upper == 1)
        Bits_Set(0x2e, t->bits);
    Bits_Set(n_upper == i ? 0x18 : 0x31, t->bits);
    if (n_upper > 1 || n_upper == i)
        Bits_Set(0x17, t->bits);
    if (n_vowel == 0 && n_letter > 0)
        Bits_Set(0x17, t->bits);
    if (n_dotletter == i) {
        Bits_Set(7, t->bits);
        Bits_Set(0x17, t->bits);
    }
    if (n_upper == 0 && n_letter == i)
        Bits_Set(0x2f, t->bits);
    if (n_punct == i) {
        if (i == 1 && s[0] == '\'')
            t->w08 = 0x23d;
        if (i > 1 || !(prevcls & 0x42))
            Bits_Set(0x43, t->bits);
        if (i > 3)
            Bits_Set(2, t->bits);
        if (i > 6 || n_cr != 0)
            Bits_Set(0x46, t->bits);
    }
    if (n_op == i && i <= 2)
        Bits_Set(0x12, t->bits);
    if (cls2_signed(t->trail) & 0x40)
        Bits_Set(0x12, t->bits);
    if (cls2_signed(t->trail) & 0x20)
        Bits_Set(0x19, t->bits);
    if (n_digit == i) {
        t->num = tv_atol(t->text);
        t->w34 = 0;
        t->is_number = 1;
        return TextIn_ClassifyNumber(self, t);
    }
    return 1;
}

/* Number tokens: guess date and time components from the value, length,
 * trailing character and the previous token's classes. */
/* @0x1002b9f0 */
int32_t TV_THISCALL TextIn_ClassifyNumber(TextIn *self, Token *t)
{
    uint32_t *bits = t->bits;
    const uint32_t *pbits = t->prev != NULL ? t->prev->bits : NULL;
    uint8_t trail;
    (void)self;

    Bits_Set(0x13, bits);
    if (t->len == 5)
        Bits_Set(0xc, bits);
    trail = t->trail;

    if (trail == '.' || trail == '/') {
        if (!Bits_Test(0x24, pbits) && t->num > 0 && t->num < 32 && t->len < 3) {
            Bits_Set(0x24, bits);
            Bits_Set(0xa, bits);
        }
        if (trail == '.') {
            Bits_Set(0x10, bits);
            if (!Bits_Test(0x29, pbits) && t->num >= 0 && t->num < 25 && t->len < 3) {
                Bits_Set(0x29, bits);
                Bits_Set(0xb, bits);
            }
        }
    } else if (trail == ':') {
        if (Bits_Test(0x29, pbits)) {
            if (t->len == 2 && t->num >= 0 && t->num < 60) {
                Bits_Set(0x27, bits);
                Bits_Set(0xb, bits);
            }
        } else if (t->len < 3 && t->num >= 0 && t->num < 25) {
            Bits_Set(0x29, bits);
            Bits_Set(0xb, bits);
        }
    } else if (trail == ',') {
        Bits_Set(0x11, bits);
        Bits_Set(0xe, bits);
    } else if (trail == '\'') {
        Bits_Set(0x2b, bits);
    }

    if (Bits_Test(0x24, pbits)) {
        if (t->num > 0 && t->num < 13 && t->len < 3) {
            Bits_Set(0x25, bits);
            Bits_Set(0xa, bits);
        }
    } else if (Bits_Test(0x25, pbits)) {
        if ((t->len == 2 && t->num > 20) || (t->len == 4 && t->num < 2020)) {
            Bits_Set(0x26, bits);
            Bits_Set(0xa, bits);
        }
    }
    if (Bits_Test(0x29, pbits) && t->len == 2 && t->num >= 0 && t->num < 60) {
        Bits_Set(0x27, bits);
        Bits_Set(0xb, bits);
    }
    t->w34 = 0;
    return 1;
}

/* ---- driver ---------------------------------------------------------------- */

/* Read and split tokens while there is room for them (at most 210 live). */
/* @0x10029c20 */
int32_t TV_THISCALL TextIn_Tokenize(TextIn *self)
{
    int32_t n = 0;
    int32_t room = 210 - self->count;
    Token *t;

    if (room < 1)
        return 1;
    while (room > n) {
        if (TextIn_ReadToken(self, &t) != 1)
            break;
        n++;
        TextIn_Split(self, &t);
        if (self->mode == 4 && t->prev != NULL && Bits_Test(0x45, t->prev->bits))
            TextIn_Mode4(self, t);
    }
    return n;
}

/* Move the expansion cursor one token on, expanding that token first unless
 * it has already been handled. */
/* @0x100253b0 */
int32_t TV_THISCALL TextIn_Advance(TextIn *self)
{
    Token *t = self->cur != NULL ? self->cur->next : self->head;
    if (t == NULL)
        return 0;
    if (self->d04 == 1 && !Bits_Test(0x44, t->bits)) {
        self->cur = t;
        return 1;
    }
    if (t->d1c != 0) {
        self->cur = t;
        return 1;
    }
    TextIn_ExpandToken(self, &t);
    self->cur = t != NULL ? t : self->head;
    return 1;
}

/* @0x10025b80 */
int32_t TV_THISCALL TextIn_ExpandToken(TextIn *self, Token **pt)
{
    char a[120], b[120];
    TextIn_Expand(self, *pt, a, b);
    return 1;
}

static void emit_token(TextIn *self, Token *t)
{
    const char *s = t->text2 != NULL ? t->text2 : t->text;
    char buf[2];
    if (s != NULL)
        TextIn_PutString(self, s);
    if (t->trail) {
        buf[0] = (char)t->trail;
        buf[1] = 0;
        TextIn_PutString(self, buf);
    }
}

/* Send expanded tokens on to the preformatter and free them.  Unless `all`,
 * the 100 tokens before the cursor are kept as context. */
/* @0x10025bb0 */
int32_t TV_THISCALL TextIn_Emit(TextIn *self, int32_t all)
{
    Token *t = self->head, *stop = self->cur;
    int32_t i;

    if (t == NULL || stop == NULL)
        return 0;
    if (!all) {
        for (i = 0; stop->prev != NULL && i < 100; i++)
            stop = stop->prev;
        while (t != NULL && t != stop) {
            emit_token(self, t);
            t = TextIn_RemoveToken(self, t, 1);
        }
    } else {
        while (t != NULL) {
            emit_token(self, t);
            t = TextIn_RemoveToken(self, t, 1);
        }
    }
    return 1;
}

/* @0x10029b70 */
int32_t TV_THISCALL TextIn_Flush(TextIn *self, int32_t final)
{
    int i;
    if (final == 0) {
        TextIn_Tokenize(self);
        if (self->input_done && self->item_done) {
            while (TextIn_Advance(self))
                ;
            TextIn_Emit(self, 1);
            return 1;
        }
        for (i = 10; i != 0; i--)
            TextIn_Advance(self);
        TextIn_Emit(self, 0);
        return 10;
    }
    while (TextIn_Tokenize(self) > 0) {
        for (i = 10; i != 0; i--)
            TextIn_Advance(self);
        TextIn_Emit(self, 0);
    }
    while (TextIn_Advance(self))
        ;
    TextIn_Emit(self, 1);
    return 1;
}

/* Symbol -> spoken word for the cp1252 punctuation that survives as a
 * one-character token. */
static const char *symbol_word(uint8_t c)
{
    switch (c) {
    case 0xa3: return " pound ";
    case 0xa2: return " cent ";
    case 0xa5: return " yen ";
    case 0xa7: return " section ";
    case 0xa9: return " copyright ";
    case 0xae: return " registered ";
    case 0xb1: return " plus or minus ";
    case 0xb6: return " paragraph ";
    case 0xbc: return " one-quarter ";
    case 0xbd: return " one-half ";
    case 0xbe: return " three-quarters ";
    case 0xd7: return " times ";
    case 0xf7: return " divided by ";
    default:   return NULL;
    }
}

static int32_t set_text2(TextIn *self, Token *t, const char *s)
{
    if (AllocString(&t->text2, (int32_t)strlen(s) + 1) == -1)
        return -1;
    strcpy(t->text2, s);
    return 0;
}

/* Expand one token in place, using its neighbours as context.  Only the
 * cases the original front end handles here are covered: a leading "minus",
 * spelled-out repeated vowels, the two embedded ESC index/mode commands,
 * cp1252 symbols, "un known"/"un sent", "AM", month names, and an emphasis
 * hold after "!".  buf1/buf2 are scratch used by deeper expansion paths;
 * only buf1[0] is touched here. */
/* @0x10025450 */
int32_t TV_THISCALL TextIn_Expand(TextIn *self, Token *t, char *buf1, char *buf2)
{
    int32_t classes[32];
    int32_t n = 0, bit = 0, i, run;
    Token *p, *q, *nt;
    const char *word;
    uint8_t c;
    (void)buf2;

    classes[0] = -1;
    if (t->d18 != NULL) {
        classes[0] = *(int32_t *)((uint8_t *)t->d18 + 0x28);
        if (classes[0] != 0)
            n = 1;
    }
    for (;;) {
        int32_t cls = (int16_t)Bits_Next(bit, t->bits);
        if (cls == 0)
            break;
        bit = cls;
        if (cls == classes[0])
            continue;
        if (n < 32)
            classes[n++] = cls;
    }
    if (n == 0)
        return 0;
    if (n > 30)
        n = 30;

    for (i = 0; i < n; i++) {
        switch (classes[i]) {
        case 0x12:
            /* leading "minus" before a number at a sentence start */
            if (strcmp(t->text, "-") != 0)
                break;
            if (t->trail != 0 && t->trail != '$')
                break;
            p = t->next;
            if (p == NULL || p->is_number == 0)
                break;
            q = t->prev;
            if (q != NULL && q->trail != ' ' &&
                (q->trail != 0 || strcmp(q->text, "\n") != 0))
                break;
            if (set_text2(self, t, "minus ") == -1)
                return 1;
            break;
        case 0x2f:
            /* a single vowel repeated (e.g. "iii") is spelled out */
            c = (uint8_t)t->text[0];
            if (c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' && c != 'y')
                break;
            if (t->len <= 2)
                break;
            for (run = 1; run < t->len; run++)
                if ((uint8_t)t->text[run] != c)
                    break;
            if (run != t->len)
                break;
            if (AllocString(&t->text2, t->len + 1) == -1)
                return 1;
            strcpy(t->text2, t->text);
            tv_strupr(t->text2);
            break;
        case 0x44:
            /* embedded ESC[nX (output mode) / ESC[nI (index mode) */
            if (t->text[3] == 'X')
                self->mode = t->text[2] - '0';
            else if (t->text[3] == 'I')
                self->d04 = t->text[2] - '0';
            break;
        default:
            break;
        }
    }

    *buf1 = 0;

    if (t->text != NULL && (uint8_t)t->text[0] > 0x7f && t->len == 1 &&
        (word = symbol_word((uint8_t)t->text[0])) != NULL) {
        if (set_text2(self, t, word) == -1)
            return 1;
    }

    if (t->text != NULL) {
        if (tv_stricmp(t->text, "unknown") == 0) {
            if (set_text2(self, t, "un known") == -1)
                return 1;
        } else if (tv_stricmp(t->text, "unsent") == 0) {
            if (set_text2(self, t, "un sent") == -1)
                return 1;
        }
    }

    if (t->len == 2 && tv_stricmp(t->text, "am") == 0) {
        p = t->prev;
        while (p != NULL && p->len == 0 && p->trail == ' ')
            p = p->prev;
        if (p != NULL && p->is_number != 0 && (p->trail == ' ' || p->trail == 0)) {
            if (set_text2(self, t, "AM") == -1)
                return 1;
        }
    }

    if (t->len == 3 && (tv_stricmp(t->text, "jan") == 0 || tv_stricmp(t->text, "mar") == 0)) {
        int match = 0;
        p = t->prev;
        if (p != NULL && p->is_number != 0 &&
            (p->trail == ' ' || p->trail == '-' || p->trail == 0)) {
            match = 1;
        } else {
            q = t->next;
            if (q != NULL && q->len == 0 && q->trail == ' ')
                q = q->next;
            if (q != NULL && q->is_number != 0 &&
                (t->trail == ' ' || t->trail == '-' || t->trail == '.' || t->trail == 0)) {
                match = 1;
                if (t->trail == '.' || t->trail == '-')
                    t->trail = ' ';
            }
        }
        if (match) {
            c = (uint8_t)t->text[0];
            if (set_text2(self, t, (c == 'J' || c == 'j') ? "January" : "March") == -1)
                return 1;
        }
    }

    if (t->trail == '!' && t->prev != NULL) {
        for (p = t->prev; p != NULL; p = p->prev) {
            if (p->trail == '.' || p->trail == '!' || p->trail == '?')
                break;
            if (p->len == 1 &&
                (p->text[0] == '"' || p->text[0] == '\\' || p->text[0] == '(')) {
                nt = TextIn_InsertAfter(self, p);
                if (nt == NULL) {
                    TextIn_Error(self, 1);
                    return 0;
                }
                if (AllocString(&nt->text, 4) != -1)
                    strcpy(nt->text, "\x1b[C");
                break;
            }
        }
    }
    return 1;
}

/* Characters that can start a quoted-mail prefix. */
/* @0x100eec10 */
extern const char g_quote_chars[];

/* Mode 4 (quoted mail): track the prefix that marks quoted lines (">", "|",
 * ": " and so on).  Once two consecutive lines carry it, the block is
 * announced, the prefixes are blanked out of the text, and the end of the
 * block is announced too.  Returns 2 when an announcement was inserted. */
/* @0x1002a450 */
int32_t TV_THISCALL TextIn_Mode4(TextIn *self, Token *t)
{
    char buf[8];
    int32_t first_sym = -1, last_sym = -1, same = 0, len, i, k;
    const char *text = t->text;

    memset(buf, 0, sizeof buf);
    if (text == NULL) {
        self->quote_count = 0;
        self->quote_prefix[0] = 0;
    } else {
        len = t->len < 5 ? t->len : 5;
        for (i = 0; i < len; i++) {
            uint8_t c = (uint8_t)text[i];
            buf[i] = (char)c;
            if ((uint8_t)self->quote_prefix[i] == c)
                same++;
            if (tv_strchr(g_quote_chars, (char)c) != NULL) {
                last_sym = i;
                if (i - same == -1)
                    first_sym = i;
            }
        }
        if (t->len < 5) {
            uint8_t c = t->trail;
            buf[i] = (char)c;
            if ((uint8_t)self->quote_prefix[i] == c)
                same++;
            if (c != 0 && tv_strchr(g_quote_chars, (char)c) != NULL) {
                last_sym = i;
                if (i - same == -1)
                    first_sym = i;
            }
        }
        buf[i + 1] = 0;
        if (last_sym < 0) {
            self->quote_prefix[0] = 0;
        } else if (self->quote_prefix[0] == 0) {
            strcpy(self->quote_prefix, buf);
            self->quote_len = (int16_t)(last_sym + 1);
            self->quote_count = 0;
        } else if (first_sym > -1) {
            self->quote_len = (int16_t)(first_sym + 1);
        } else {
            self->quote_len = (int16_t)(last_sym + 1);
            strcpy(self->quote_prefix, buf);
            self->quote_count = 0;
        }
    }

    if (self->in_quote != 0) {
        if (first_sym <= -1) {
            TextIn_InsertText(self, t, ". Achtung: Ende des eingesetzten textes.  ", -1);
            t->prev->d1c = 1;
            self->in_quote = 0;
            self->quote_count = 0;
            return 2;
        }
        if (t->len <= self->quote_len) {
            Bits_Set(2, t->bits);
            Bits_Set(0x43, t->bits);
            return 1;
        }
        for (k = 1; k <= self->quote_len; k++)
            t->text[k - 1] = ' ';
        return 1;
    }

    if (first_sym <= -1)
        return 1;
    self->quote_count++;
    if (self->quote_count <= 1)
        return 1;

    for (i = 0; i < 3;) {
        while (!Bits_Test(0x45, t->prev->bits))
            t = t->prev;
        if (t->len > self->quote_len) {
            for (k = 1; k <= self->quote_len; k++)
                t->text[k - 1] = ' ';
        } else {
            Bits_Set(0x43, t->bits);
            Bits_Set(2, t->bits);
        }
        t = t->prev;
        i++;
    }
    TextIn_InsertText(self, t, ". Achtung: Anfang des eingesetzten textes.  ", -1);
    self->in_quote = 1;
    return 2;
}
