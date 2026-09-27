/*
 * Stage 0: the text rule interpreter.
 *
 * A byte-coded rule program (g_10120a10) walks the character nodes the input
 * stage produced.  Each rule is a sequence of condition opcodes followed by a
 * header byte holding the action count and a skip distance, then the action
 * opcodes.  Conditions inspect the node under the read cursor (scan), the
 * per-stage flags, rule variables, or whole words looked up in the word
 * lists; actions rewrite, insert and delete nodes, emit replacement text,
 * and call other rules through a small call stack.
 *
 * The interpreter runs in steps: when it runs out of nodes it saves its
 * place (s0_need_test / s0_nactions) and returns, so the caller can refill
 * the work list first.
 */
#include "engine.h"

/* The rule program. */
/* @0x10120a10 */
extern const uint8_t g_stage0_rules[];
/* Literal pools the rule program indexes with a 16-bit offset. */
/* @0x10121618 */
extern const char g_stage0_strings[];
/* Characters that count as "word-like" for condition opcode 3. */
/* @0x10120a00 */
extern const char g_stage0_wordchars[];
/* Word lists, indexed by the selector byte of the match opcodes; each is a
 * NULL-terminated array of "word\0replacement\0" strings. */
/* @0x101209a8 */
extern const tv_ref g_stage0_words[256];
/* Flag bits per condition opcode 2 parameter. */
/* @0x100f83a0 */
extern const uint32_t g_s0_flag_lo[16];
/* @0x100f8360 */
extern const uint32_t g_s0_flag_hi[256];
/* Per-character attributes, in three banks of 128 selected by bits 7 and 8 of
 * the index: the character as it arrives from stage 0, the same character as
 * a phoneme symbol, and two more banks the stage 1 prosody and lexicon code
 * use.  The
 * engine sign-extends the character, so a byte >= 0x80 indexes the 128 bytes
 * *before* the table. */
/* @0x100c8aa0 */
extern const uint8_t g_phone_attr[0x280];

/* g_phone_attr with the signed index the original uses (-128..255): a
 * negative one reaches the bytes stored in front of the table, which are
 * there in both builds. */
uint8_t Phone_Attr(int32_t idx)
{
    return g_phone_attr[idx];
}

static uint8_t phone_attr_signed(uint8_t c)
{
    return Phone_Attr((int8_t)c);
}

/* Character classes 4..19 used by the condition opcodes. */
/* @0x100313d0 */
uint8_t TV_CDECL Stage0_CharClass(int32_t cls, uint8_t c)
{
    /* @0x10031474 */
    extern const uint8_t g_s0_class_index[16];
    int32_t i = cls - 4;
    if ((uint32_t)i > 15)
        return 0;
    switch (g_s0_class_index[i]) {
    case 0:
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
    case 1:
        return c >= '0' && c <= '9';
    case 2:
        return c >= 'a' && c <= 'z';
    case 3:
        return c >= 'A' && c <= 'Z';
    case 4:
        return (int8_t)c < 0x20;
    case 5:
        return c == '.';
    default:
        return 0;
    }
}

/* Whether a character is *not* one of the phoneme letters the rules skip. */
/* @0x10031490 */
uint8_t TV_CDECL Stage0_IsPlain(char c)
{
    /* @0x100314cc */
    extern const uint8_t g_s0_plain_index[0x36];
    int32_t i = (int32_t)c - 0x43;
    if ((uint32_t)i > 0x35)
        return 1;
    return g_s0_plain_index[i] == 5;
}

/* Match one of the words of list `list` against the nodes from the match
 * start onwards.  On success s0_word_data points at the replacement text
 * stored after the word. */
/* @0x10031310 */
uint8_t TV_THISCALL Stage0_MatchWord(Engine *self, uint8_t list, uint8_t fold_case)
{
    StageCtx *st = &self->stage_ctx[0];
    const tv_ref *words = TV_REF(tv_ref, g_stage0_words[list]);
    const char *w;
    Node *n;

    for (;;) {
        w = TV_REF(char, *words);
        if (!TV_REF_OK(*words))
            return 0;
        words++;
        n = st->d14;
        for (;;) {
            uint8_t c;
            st->d18 = n;
            n = st->d18;
            c = n->value;
            if (fold_case == 1 && c >= 'A' && c <= 'Z')
                c += 0x20;
            if (NODE_TYPE(n) == 1 && (uint8_t)*w == c) {
                w++;
                if (*w != 0 && st->scan != n) {
                    n = n->next;
                    continue;
                }
            }
            break;
        }
        if (*w == 0 && st->scan == st->d18) {
            self->s0_word_data = (const uint8_t *)(w + 1);
            if (list == 1 || list == 0xd || list == 0xe)
                self->s0_1c1c = 1;
            return 1;
        }
        w++;
    }
}

/* Emit a node carrying `value` just before the write cursor, moving any
 * pending stress/accent onto it. */
/* @0x10032170 */
void TV_THISCALL Stage0_Emit(Engine *self, int32_t type, uint8_t value)
{
    StageCtx *st = &self->stage_ctx[0];
    Node *n;

    if (st->ctl != NULL) {
        n = Engine_NodeAlloc(self, st->ctl, 0, type, value);
    } else {
        n = Engine_NodeAlloc(self, self->work_tail->prev, 1, type, value);
        st->last = n;
        if (st->first == NULL)
            st->first = n;
    }
    if ((phone_attr_signed(n->value) & 8) && (self->s0_pending_ptr->flags & 0x20)) {
        n->flags |= 0x20;
        n->b15 = self->s0_pending_ptr->b15;
        self->s0_pending_ptr->b15 = 0;
        n->arg = self->s0_pending_ptr->arg;
        self->s0_pending_ptr->arg = 0;
        self->s0_pending_ptr->flags &= ~0x20u;
    }
    if (n->value >= 'A' && n->value <= 'Z')
        n->b14 = 1;
}

/* Hand the finished nodes to stage 1 and stop for now. */
/* @0x100320f0 */
uint8_t TV_THISCALL Stage0_Finish(Engine *self, uint8_t done)
{
    StageCtx *st = &self->stage_ctx[0];

    if (done) {
        st->cur = st->ctl;
    } else {
        st->cur = st->first;
        if (st->ctl != st->first) {
            for (;;) {
                Node *n = st->cur;
                uint32_t t = NODE_TYPE(n);
                if (t != 0 && (t != 3 || n->value != ']'))
                    break;
                st->cur = Engine_StageNext(self, n);
                if (st->cur == st->ctl)
                    break;
            }
        }
    }
    if (Engine_StageEnd(self))
        done = 1;
    return done;
}

/* @0x10031560 */
uint8_t TV_THISCALL Stage0_Run(Engine *self)
{
    StageCtx *st = &self->stage_ctx[0];
    const uint8_t *ip;
    uint8_t matched;
    int32_t i;

    if (!Engine_StageBegin(self, st))
        return 0;
    if (st->d14 == NULL)
        st->d14 = st->ctl;

    for (;;) {
        if (self->s0_need_test) {
            matched = 0;
            ip = self->s0_ip;
            switch (ip[0]) {
            case 0:
                if (st->p_1c != 0)
                    matched = 1;
                break;
            case 1:
                if (st->p_20 == 0)
                    matched = 1;
                break;
            case 2: {
                uint8_t k;
                ip++;
                self->s0_ip = ip;
                k = ip[0];
                if (k < 0x10) {
                    if (st->p_34 & g_s0_flag_lo[k])
                        matched = 1;
                } else if (st->p_38 & g_s0_flag_hi[k]) {
                    matched = 1;
                }
                break;
            }
            case 3: {
                uint8_t c;
                if (st->p_1c == 2) {
                    if (st->scan == st->last)
                        return Stage0_Finish(self, 0);
                    Stage0_Phonetic(self);
                }
                c = st->scan->value;
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '1' && c <= '5')) {
                    matched = 1;
                } else {
                    for (i = 0; g_stage0_wordchars[i] != 0; i++) {
                        if ((uint8_t)g_stage0_wordchars[i] == c) {
                            matched = 1;
                            break;
                        }
                        if (c == '\\')
                            matched = 1;
                    }
                }
                break;
            }
            case 4:
            case 5:
            case 6:
            case 7:
                if (Stage0_CharClass(ip[0], st->scan->value))
                    matched = 1;
                break;
            case 8: {
                int32_t steps = 0;
                st->d18 = st->scan;
                for (;;) {
                    Node *n;
                    uint8_t c;
                    if (st->d18 == st->last)
                        return Stage0_Finish(self, 0);
                    st->d18 = st->d18->next;
                    n = st->d18;
                    c = n->value;
                    if (n->flags & 7) {
                        if (Stage0_CharClass(self->s0_ip[1], c)) {
                            matched = 1;
                            break;
                        }
                        if (c != ' ' && c != '~' && c != '`')
                            break;
                    } else {
                        if (!Stage0_IsPlain((char)c)) {
                            if (Stage0_CharClass(self->s0_ip[1], 'a'))
                                matched = 1;
                            break;
                        }
                    }
                    steps++;
                    if (steps >= 0x14)
                        break;
                }
                self->s0_ip++;
                break;
            }
            case 9: {
                uint8_t idx, v;
                ip++;
                self->s0_ip = ip;
                idx = ip[0];
                ip++;
                v = self->s0_vars[idx];
                self->s0_ip = ip;
                if (ip[0] == v)
                    matched = 1;
                break;
            }
            case 10: {
                uint8_t idx, v;
                ip++;
                self->s0_ip = ip;
                idx = ip[0];
                ip++;
                v = self->s0_vars[idx];
                self->s0_ip = ip;
                if (ip[0] < v)
                    matched = 1;
                break;
            }
            case 11:
                ip++;
                self->s0_ip = ip;
                if ((int32_t)(int8_t)st->scan->value == (int32_t)ip[0])
                    matched = 1;
                break;
            case 12:
                ip++;
                self->s0_ip = ip;
                matched = Stage0_MatchWord(self, ip[0], 1);
                break;
            case 13:
                if (st->last == st->scan)
                    return Stage0_Finish(self, 0);
                st->scan = Engine_StageNext(self, st->scan);
                if (!(st->scan->flags & 7))
                    matched = 1;
                break;
            case 14:
                if (st->scan == NULL)
                    return Stage0_Finish(self, 0);
                if (!(st->scan->flags & 7))
                    matched = 1;
                break;
            case 15:
                matched = 1;
                break;
            case 16:
                if (Stage0_IsPlain((char)st->scan->value))
                    matched = 1;
                break;
            case 17: {
                uint32_t off;
                const char *p;
                uint8_t c;
                ip++;
                self->s0_ip = ip;
                off = (uint32_t)ip[0] << 8;
                ip++;
                self->s0_ip = ip;
                off |= ip[0];
                p = g_stage0_strings + off;
                if (*p != 0) {
                    c = st->scan->value;
                    while ((uint8_t)*p != c) {
                        p++;
                        if (*p == 0)
                            break;
                    }
                    if ((uint8_t)*p == c)
                        matched = 1;
                }
                break;
            }
            case 18:
                ip++;
                self->s0_ip = ip;
                matched = Stage0_MatchWord(self, ip[0], 0);
                break;
            default:
                break;
            }

            /* rule header: action count and skip distance */
            ip = self->s0_ip;
            self->s0_need_test = 0;
            ip++;
            self->s0_ip = ip;
            if (matched) {
                self->s0_nactions = (uint8_t)(ip[0] >> 4);
                self->s0_skip = (uint8_t)(ip[0] & 0xf);
            } else {
                self->s0_skip = 0;
                self->s0_nactions = (uint8_t)(ip[0] & 0xf);
                ip += ip[0] >> 4;
                self->s0_ip = ip;
            }
            ip++;
            self->s0_ip = ip;
        }

        if (self->s0_nactions != 0) {
            for (;;) {
                uint8_t op;
                ip = self->s0_ip;
                op = ip[0];
                if (op & 0x80) {
                    /* call (0xc0) or jump (0x80) to another rule */
                    if ((op & 0xc0) != 0x80) {
                        self->s0_sp->ip = ip;
                        self->s0_sp->nactions = self->s0_nactions;
                        self->s0_sp->skip = self->s0_skip;
                        self->s0_sp++;
                        op = ip[0];
                    }
                    op &= 0x3f;
                    ip++;
                    self->s0_ip = ip;
                    self->s0_nactions = 1;
                    self->s0_skip = 0;
                    self->s0_ip = g_stage0_rules + (((uint32_t)op << 8) | ip[0]);
                    break;
                }
                if (op <= 0x16) {
                    switch (op) {
                    case 0:
                    case 1: {
                        uint32_t off;
                        const char *p;
                        ip++;
                        self->s0_ip = ip;
                        off = (uint32_t)ip[0] << 8;
                        ip++;
                        self->s0_ip = ip;
                        off |= ip[0];
                        p = g_stage0_strings + off;
                        while (*p != 0) {
                            Stage0_Emit(self, self->s0_ip[-2] == 1 ? 3 : 2, (uint8_t)*p);
                            p++;
                        }
                        self->s0_nactions -= 2;
                        break;
                    }
                    case 2:
                        st->scan = Engine_StageNext(self, st->scan);
                        st->d14 = st->scan;
                        st->ctl = st->scan;
                        break;
                    case 3: {
                        Node *m;
                        if (self->s0_vars[2] != 0)
                            st->first->flags |= 0x40;
                        if (st->cur != NULL && st->cur->value == '-') {
                            m = st->ctl->prev;
                            if (m->value == '&')
                                m->b18 = '-';
                        }
                        for (;;) {
                            Node *n = st->ctl;
                            uint8_t c = n->value;
                            if (c == '\'') {
                                n->value = '@';
                            } else if (c == '-') {
                                n->value = '&';
                                st->ctl->b18 = c;
                            } else if (c >= 'a' && c <= 'z') {
                                n->value = (uint8_t)(c - 0x20);
                            } else if (c >= 'A' && c <= 'Z') {
                                n->b14 = 1;
                            }
                            st->ctl->flags = (st->ctl->flags & ~5u) | 2;
                            if (st->scan == st->ctl)
                                break;
                            st->ctl = st->ctl->next;
                        }
                        st->scan = Engine_StageNext(self, st->scan);
                        st->d14 = st->scan;
                        st->ctl = st->scan;
                        break;
                    }
                    case 4:
                        for (;;) {
                            Node *n = st->ctl;
                            if (n->flags & 7)
                                n->flags = (n->flags & ~4u) | 3;
                            if (st->scan == st->ctl)
                                break;
                            st->ctl = st->ctl->next;
                        }
                        st->scan = Engine_StageNext(self, st->scan);
                        st->d14 = st->scan;
                        st->ctl = st->scan;
                        break;
                    case 5:
                        while (*self->s0_word_data != 0) {
                            Stage0_Emit(self, 3, *self->s0_word_data);
                            self->s0_word_data++;
                        }
                        break;
                    case 6: {
                        uint8_t idx;
                        ip++;
                        self->s0_ip = ip;
                        idx = ip[0];
                        ip++;
                        self->s0_ip = ip;
                        self->s0_vars[idx] = ip[0];
                        self->s0_nactions -= 2;
                        break;
                    }
                    case 7:
                        ip++;
                        self->s0_ip = ip;
                        self->s0_vars[ip[0]]++;
                        self->s0_nactions--;
                        break;
                    case 8:
                        ip++;
                        self->s0_ip = ip;
                        self->s0_vars[ip[0]]--;
                        self->s0_nactions--;
                        break;
                    case 9:
                        st->d14 = st->ctl;
                        st->scan = st->ctl;
                        break;
                    case 10:
                        st->scan = Engine_StageNext(self, st->scan);
                        break;
                    case 11:
                        if (st->scan == NULL)
                            st->scan = st->last;
                        else
                            st->scan = st->scan->prev;
                        break;
                    case 12:
                        if (st->ctl == st->scan) {
                            st->d14 = Engine_StageNext(self, st->ctl);
                            st->ctl = st->d14;
                        }
                        st->scan = Engine_NodeFree(self, st->scan, 1);
                        break;
                    case 13:
                        while (st->ctl != st->scan)
                            st->ctl = Engine_NodeFree(self, st->ctl, 1);
                        st->ctl = Engine_NodeFree(self, st->ctl, 1);
                        st->d14 = st->ctl;
                        st->scan = st->ctl;
                        break;
                    case 14:
                        Engine_NodeAlloc(self, st->scan, 1, 2, ip[1]);
                        st->scan = st->scan->next;
                        self->s0_ip++;
                        self->s0_nactions--;
                        break;
                    case 15:
                        ip++;
                        self->s0_ip = ip;
                        st->scan->value = ip[0];
                        self->s0_nactions--;
                        break;
                    case 16:
                        ip++;
                        self->s0_ip = ip;
                        Stage0_MatchWord(self, ip[0], 1);
                        self->s0_nactions--;
                        break;
                    case 17:
                        if (self->s0_1b3d == 0) {
                            self->s0_1b3d = 1;
                            return Stage0_Finish(self, 1);
                        }
                        self->s0_1b3d = 0;
                        break;
                    case 18:
                        Engine_RunControl(self);
                        break;
                    case 19: {
                        S0Frame *f;
                        self->s0_sp--;
                        f = self->s0_sp;
                        self->s0_ip = f->ip + 1;
                        self->s0_nactions = (uint8_t)(f->nactions - 1);
                        self->s0_skip = f->skip;
                        break;
                    }
                    case 20:
                    case 21: {
                        int32_t vals[2];
                        int32_t *v = vals;
                        Node *n;
                        uint8_t c;
                        if (op == 20)
                            st->ctl->flags |= 0x80;
                        if (self->s0_ip[0] == 0x15)
                            st->ctl->flags &= ~0x80u;
                        n = st->ctl;
                        st->d18 = n->next;
                        vals[0] = -1;
                        vals[1] = -1;
                        for (;;) {
                            st->d18 = st->d18->next;
                            c = st->d18->value;
                            if (c >= '0' && c <= '9') {
                                if (*v < 0)
                                    *v = c - '0';
                                else
                                    *v = *v * 10 + c - '0';
                                if (*v <= 0xff)
                                    continue;
                            } else if (c == ';') {
                                v++;
                                if (v < vals + 2)
                                    continue;
                            }
                            break;
                        }
                        if (vals[0] < 0)
                            vals[0] = 0;
                        else if (vals[0] == 0)
                            vals[0] = 2;
                        if (vals[1] < 0)
                            vals[1] = 0;
                        if (c == '!') {
                            if (vals[0] > 300)
                                break;
                            if (vals[1] > 160 || vals[1] < 40) {
                                if (vals[1] != 0)
                                    break;
                            }
                            if (!(phone_attr_signed(n->value) & 0x80))
                                break;
                            if (vals[0] != 0)
                                n->b15 = (uint8_t)((vals[0] - 100) >> 1);
                            if (vals[1] != 0)
                                st->ctl->arg = (uint32_t)(vals[1] - 100);
                        } else if (c == '/') {
                            if (vals[0] < 50 && vals[0] > 2)
                                break;
                            if (vals[0] > 200 || vals[1] > 60)
                                break;
                            if (!(phone_attr_signed(n->value) & 0x80))
                                break;
                            n->b15 = (uint8_t)(vals[0] >> 1);
                            st->ctl->arg = (uint32_t)vals[1];
                        }
                        break;
                    }
                    case 22:
                        st->d14 = st->scan;
                        break;
                    default:
                        break;
                    }
                }
                self->s0_ip++;
                self->s0_nactions--;
                if (self->s0_nactions == 0)
                    break;
            }
        }
        self->s0_need_test = 1;
        self->s0_ip += self->s0_skip;
    }
}
