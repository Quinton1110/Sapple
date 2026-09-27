/*
 * Stage 1: the per-vowel pass.
 *
 * Stage1_Pronounce calls this for every vowel of every word.  It looks at the
 * phonemes either side -- the immediate neighbours and the nearest vowels
 * across the syllable boundaries -- and applies a long list of allophone
 * rules: "L" goes dark or syllabic, "H" drops, stops get flapped or
 * unreleased, vowels colour before "R", glides appear and vanish.  Most rules
 * end by rewriting the node's symbol, deleting it, or inserting one; every
 * path then runs a shared clean-up pass.  The node the caller should carry on
 * from is returned.
 */
#include "engine.h"

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

static uint8_t attr_lo(uint8_t c) { return Phone_Attr((int8_t)c); }
static uint8_t attr_hi(uint8_t c)
{
    return Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x80));
}
static uint8_t attr_x(uint8_t c)
{
    return Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x100));
}

/* @0x10060a60 */
Node *TV_THISCALL Stage1_Vowel(Engine *self, Node *n)
{
    StageCtx *st = &self->stage_ctx[1];
    Node *nxt, *nxt_saved, *nxt2, *prv, *prv_saved;
    Node *nextv = NULL;             /* [esp+0x40] as a pointer */
    Node *ret = NULL;               /* [esp+0x14] */
    Node *p, *q, *t;
    int32_t ch_next = 0;            /* [esp+0x18] */
    int32_t ch_prev = 0;            /* [esp+0x1c] */
    int32_t ch_prevv = 0;           /* [esp+0x20] */
    int32_t ch_nextv = 0;           /* [esp+0x2c] */
    int32_t ch_next2 = 0;           /* [esp+0x3c] */
    int32_t did_prev = 0;           /* [esp+0x44] */
    int32_t did_next = 0;           /* [esp+0x48] */
    uint8_t at_accent = 0;          /* [esp+0x0e] */
    uint8_t at_start = 0;           /* [esp+0x0f] */
    uint8_t c = n->value;           /* bl */
    uint8_t v40 = 0;                /* [esp+0x40] as a byte */
    uint8_t attr2 = 0;              /* [esp+0x28] as a byte */
    int32_t i180 = 0;               /* [esp+0x28] as a pointer */
    int32_t ix;                     /* ecx in the vowel branch */
    int16_t ch16;
    uint8_t d;
    uint32_t f;

    p = self->s1_next_start;
    if (p != NULL && (attr_hi(p->value) & 0x40))
        at_accent = 1;

    nxt = nxt_saved = Node_NextWord(self, n);
    prv = prv_saved = Node_PrevBoundary(self, n);
    if (nxt != NULL)
        ch_next = (int8_t)nxt->value;
    if (prv != NULL)
        ch_prev = (int8_t)prv->value;
    nxt2 = Node_NextWord(self, nxt);
    if (nxt2 != NULL)
        ch_next2 = (int8_t)nxt2->value;

    /* the nearest vowel behind, across the syllable boundaries */
    while (prv != NULL) {
        if (attr_hi(prv->value) & 0x40)
            break;
        if (attr_lo(prv->value) & 0x80) {
            ch_prevv = (int8_t)prv->value;
            break;
        }
        prv = Node_PrevBoundary(self, prv);
    }

    if (prv_saved == NULL ||
        ((prv_saved->value == '&' || prv_saved->value == '%') &&
         prv_saved->prev->value == 0))
        at_start = 1;

    if (ch_prevv == 0)
        ch_prevv = self->s1_1c38;
    else
        self->s1_1c38 = 0;

    /* and the nearest vowel ahead */
    while (nxt != NULL) {
        if (attr_hi(nxt->value) & 0x40)
            break;
        if (NODE_TYPE(nxt) == 3 && (attr_lo(nxt->value) & 0x80)) {
            ch_nextv = (int8_t)nxt->value;
            nextv = nxt;
            break;
        }
        nxt = Node_NextWord(self, nxt);
    }

    /* "the press" wants a pause in front of it */
    if (at_accent != 0) {
        q = n->prev;
        if (q != NULL) {
            p = q->prev;
            if (p != NULL && p->value == 'S' && (p = p->prev) != NULL &&
                p->value == 'e' && (p = p->prev) != NULL && p->value == 'R' &&
                (p = p->prev) != NULL && p->value == 'P' &&
                (p = p->prev) != NULL && p->value == '&' &&
                (p = p->prev) != NULL && !(attr_lo(p->value) & 0x80)) {
                if (q->b14 == 2 || Stage1_VowelAux(self)) {
                    ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
                    ret->arg = 0x1b;
                    goto post;
                }
            }
        }
    }

    ch16 = (int16_t)(int8_t)c;
    if (!(Phone_Attr((int16_t)(ch16 | 0x80)) & 0x20))
        goto vowel_branch;

    /* ================= consonants ====================================== */

    if (ch_prevv == 'q' && st->pitch != 0 && (Phone_Attr(ch16) & 4) &&
        prv_saved != NULL && prv_saved->prev != NULL &&
        prv_saved->prev->prev != NULL) {
        p = prv_saved->prev->prev;
        if (!(attr_x(p->value) & 2) || c != 'x') {
            n->flags |= 0x40u;
            did_prev = 1;
        }
    }

    if (c == 'Q') {
        p = nxt_saved;
        if (attr_lo(p->value) & 0x80) {
            if ((attr_x(p->value) & 2) && st->pitch != 0)
                nextv->flags |= 0x40u;
            ret = Engine_NodeFree(self, n, 0);
            goto post;
        }
        n->value = ' ';
        goto post;
    }

    if (c == 'Z' && !(Phone_Attr(ch_nextv) & 4) && ch_next != '%' &&
        ch_next != '&' && !(Phone_Attr(ch_next | 0x80) & 0x40)) {
        n->value = 'S';
        goto post;
    }

    if (ch_next == '[' && n->next->next != NULL && n->next->next->value == c) {
        ret = Engine_NodeFree(self, n, 1);
        goto post;
    }

    if (c == 'x' && prv_saved != NULL &&
        (prv_saved->value == '&' || prv_saved->value == '%') &&
        prv_saved->prev != NULL && prv_saved->prev->value == 'D' &&
        prv_saved->prev->prev != NULL && prv_saved->prev->prev->value == 'N') {
        ret = Engine_NodeFree(self, n, 0);
        did_prev = 1;
        goto post;
    }

    if ((c == 'L' || c == 'j') && ch_next == '[' && ch_nextv == 'L' &&
        ch_prevv != '@' && ch_prevv != '|')
        Engine_NodeFree(self, nextv, 0);

    if (c == 'L') {
        if (ch_prevv == 'L' &&
            (ch_prev == '&' || ch_prev == '%' || ch_prev == '[')) {
            n->value = 'j';
            goto post;
        }
        if (ch_prevv == '@' && (ch_nextv == 'D' || ch_nextv == 'E') &&
            ch_next != '&' && ch_next != '%' && ch_prev != '&' &&
            ch_prev != '%') {
            n->value = 'j';
            goto post;
        }
        if ((ch_prev == '@' || ch_prev == '|') && ch_next == 'Z' &&
            ((attr_lo(nxt2->value) & 8) || (attr_hi(nxt2->value) & 1))) {
            n->value = 'j';
            goto post;
        }
        if ((Phone_Attr(ch_prev) & 1) && nxt_saved != NULL &&
            !(nxt_saved->flags & 0x18u)) {
            if (ch_prev == 'y' &&
                (ch_next == '&' || ch_next == '%' ||
                 (Phone_Attr(ch_next | 0x80) & 0x40))) {
                n->value = 'l';
                goto post;
            }
            if (ch_prev == 'b' && prv_saved->prev->value == 'L' &&
                (Phone_Attr(ch_next | 0x100) & 2))
                goto post;
            n->value = 'j';
            goto post;
        }
    }

    i180 = (int32_t)(int16_t)(ch16 | 0x180);
    if ((Phone_Attr(i180) & 1) && (ch_prev == '&' || ch_prev == '%') &&
        (n->flags & 0x20u) && st->pitch != 0 && ch_prevv == 0 &&
        st->rate_index < 0x13)
        n->flags |= 0x40u;

    if (c == 'N' && ch_next == '[' && (Phone_Attr(ch_nextv | 0x100) & 0x80) &&
        st->rate_index > 9) {
        n->value = '~';
        goto post;
    }

    q = NULL;
    if (nxt_saved != NULL)
        q = Engine_StageNext(self, nxt_saved);

    if (c == 'H' && prv_saved != NULL) {
        int32_t drop = 0;

        if (st->rate_index > 0xd && (attr_lo(prv_saved->value) & 8) &&
            !(n->flags & 0x20u) &&
            (Phone_Attr(ch_prevv | 0x80) & 0x20))
            drop = 1;
        if (!drop && !(Phone_Attr(ch_next | 0x100) & 2) &&
            !(ch_next == 'Y' && q != NULL && q->value == 'b'))
            drop = 1;
        if (drop) {
            ret = Engine_NodeFree(self, n, 0);
            goto post;
        }
    }

    if (st->rate_index > 6 && (Phone_Attr(ch_prevv) & 4)) {
        if (c == 'H') {
            n->value = 'd';
            did_prev = 1;
            goto post;
        }
        if (c == 'h') {
            n->value = 'W';
            did_prev = 1;
            goto post;
        }
    }

    if ((c == 'L' || (Phone_Attr(ch16) & 0x10)) &&
        (Phone_Attr(ch_nextv | 0x100) & 2)) {
        if (ch_prev == '%' || ch_prev == '&' || ch_prev == '[') {
            if (prv_saved != NULL && prv_saved->prev != NULL &&
                prv_saved->prev->prev != NULL) {
                d = prv_saved->prev->value;
                if ((d == 'z' && prv_saved->prev->prev->value == 'J') ||
                    (d == 's' && prv_saved->prev->prev->value == 'C')) {
                    ret = Engine_NodeAlloc(self, prv_saved, 0, 3, ' ');
                    ret->arg = 3;
                    goto post;
                }
            }
        } else if (prv_saved != NULL && prv_saved->prev != NULL) {
            d = prv_saved->value;
            if ((d == 'z' && prv_saved->prev->value == 'J') ||
                (d == 's' && prv_saved->prev->value == 'C')) {
                ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
                ret->arg = 3;
                goto post;
            }
        }
    }

    if (ch_prevv == 'S') {
        if (c == 'M') {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            if (ch_next == '.')
                ret->arg = 4;
            else
                ret->arg = 3;
            goto post;
        }
        if (c == 'N' || (Phone_Attr(i180) & 1)) {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            ret->arg = 3;
            goto post;
        }
    }
    if (ch_prevv == 'Z') {
        if (c == 'M') {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            if (ch_next == '.')
                ret->arg = 3;
            else
                ret->arg = 2;
            ret->arg = 2;
            goto post;
        }
        if (c == 'N') {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            ret->arg = 2;
            goto post;
        }
        if (Phone_Attr(i180) & 1) {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            ret->arg = 2;
            goto post;
        }
    }

    if (!(Phone_Attr(ch16) & 0x20))
        goto post;

    /* a stop after "S" is unaspirated, i.e. spelled as its voiced partner */
    if (ch_prev == 'S' || (ch_prev == '[' && ch_prevv == 'S')) {
        if (c == 'P' || c == 'K' || (c == 'T' && ch_next != 'R')) {
            if (nxt_saved != NULL && (nxt_saved->flags & 0x18u)) {
                d = 0;
                if (c == 'T')
                    d = 'D';
                else if (c == 'P')
                    d = 'B';
                else if (c == 'K')
                    d = 'G';
                if (d != 0) {
                    n->value = d;
                    goto post;
                }
            }
        }
    }

    if ((c == 'D' || c == 'T') && ch_next == 'R' && ch_prev != 'S') {
        if (c == 'D') {
            n->value = 'J';
            c = 'J';
        } else {
            n->value = 'C';
            c = 'C';
        }
    }

    if (c == 'D' && (ch_next == '&' || ch_next == '%') && ch_nextv == 'Y' &&
        self->s1_next_start->b15 == 1) {
        n->value = 'J';
        c = 'J';
    }

    if (c == 'T' && (ch_next == '&' || ch_next == '%') && ch_nextv == 'Y' &&
        self->s1_next_start->b15 == 1) {
        int32_t skip = 0;

        if (prv_saved != NULL && prv_saved->prev != NULL &&
            prv_saved->value != 's' && prv_saved->prev->value != 'C') {
            n->value = 'C';
            c = 'C';
        } else {
            skip = 1;
        }
        (void)skip;
        ret = Engine_NodeFree(self, nextv, 1);
    }

    {
        d = 0;
        if (c == 'C' && ch_next != 's')
            d = 's';
        else if (c == 'J' && ch_next != 'z')
            d = 'z';
        if (d != 0) {
            ret = Engine_NodeAlloc(self, n, 1, 3, d);
            f = ret->flags;
            ret->flags = (f & ~0x20u) | (n->flags & 0x20u);
            goto post;
        }
    }

    if (nxt_saved == NULL)
        goto t_after_u;
    if (c != 'D' && c != 'T')
        goto c_is_x;
    if (ch_prev == '~')
        goto t_after_u;

    if (c == 'T' && (Phone_Attr(ch_prevv) & 0x20) &&
        (ch_next == '&' || ch_next == '%') &&
        (Phone_Attr(ch_nextv | 0x100) & 2)) {
        n->value = 'D';
        goto post;
    }
    if ((c == 'T' && ch_next == 'C') || (c == 'D' && ch_next == 'J'))
        goto drop_stop;

    {
        int32_t rate = st->rate_index;

        if (rate > 0xd && (Phone_Attr(ch_nextv | 0x80) & 0x20) &&
            !(Phone_Attr(ch_prev) & 8) && ch_nextv != 'R' && ch_nextv != 'W') {
            if (c == 'T' && (ch_prevv == 'F' || ch_prevv == 'S' ||
                             ch_prevv == 'P' || ch_prevv == 'K'))
                goto drop_stop;
            if (c == 'D' && ch_prevv == 'N')
                goto drop_stop;
            if (rate >= 0x13 && (ch_prevv == 's' || ch_prevv == 'z' ||
                                 ch_prevv == 'V'))
                goto drop_stop;
        }
        if (rate > 9 && ch_next == '[' && c == 'T' &&
            (ch_prevv == 'N' || ch_prevv == 'n') && ch_nextv == 'S')
            goto drop_stop;

        v40 = attr_lo((uint8_t)ch_prev);
        attr2 = (uint8_t)(v40 & 2);
        if (attr2 != 0) {
            if (prv_saved != NULL && (prv_saved->flags & 0x20u) &&
                (Phone_Attr(ch_next | 0x80) & 0x10) && ch_next2 == 'N') {
                if (c == 'T') {
                    n->value = 'q';
                    goto post;
                }
                n->value = 't';
                n->b18 = c;
                goto post;
            }
            if (c == 'T') {
                int32_t glot = 0;

                if ((Phone_Attr(ch_next) & 8) &&
                    (Phone_Attr(ch_nextv) & 2) &&
                    !(Phone_Attr(ch_nextv) & 1) && ch_nextv != 'H')
                    glot = 1;
                if (!glot && (ch_nextv == 'D' || ch_nextv == 'L'))
                    glot = 1;
                if (glot) {
                    n->value = 'q';
                    did_next = 1;
                    goto post;
                }
            }
        }
        if (c == 'T' && ch_nextv == 'x' && (Phone_Attr(ch_prev | 0x100) & 2)) {
            n->value = 'q';
            goto post;
        }

        if (rate > 6 && st->p_20 != 0) {
            d = (uint8_t)ch_nextv;
            if ((Phone_Attr(ch_prev | 0x100) & 2) && prv_saved != NULL &&
                prv_saved->prev != NULL && prv_saved->prev->value != 't' &&
                ((attr_x(d) & 2) || d == 'l') &&
                !(nxt_saved->flags & 0x20u) &&
                ((Phone_Attr((int16_t)((int16_t)(int8_t)d | 0x200)) & 0x40) ||
                 d == 'E' || d == 'O')) {
                if (ch_next2 != 'N' ||
                    (attr_lo(nxt2->next->value) & 0x80)) {
                    n->value = 't';
                    n->b18 = c;
                    goto post;
                }
            }
            if (c == 'T' && attr2 != 0 &&
                ((ch_next == '&' || ch_next == '%') ||
                 (ch_next == '[' && ch_prev != 'L'))) {
                if ((Phone_Attr(ch_nextv | 0x100) & 2) || ch_nextv == 'H') {
                    uint32_t cls = self->s1_word_start->b15;

                    if (cls == 0 || cls == 6 || cls == 0xb) {
                        if ((Phone_Attr(ch_prevv) & 0x10) || ch_prevv == 'j') {
                            int32_t ok = 1;

                            if (ch_nextv == 'H' && prv_saved != NULL &&
                                prv_saved->prev != NULL &&
                                !(prv_saved->prev->flags & 0x20u))
                                ok = 0;
                            if (ok && cls != 0xb && cls != 2)
                                goto keep_t;
                        }
                    }
                    n->value = 't';
keep_t:
                    did_next = 1;
                    n->b18 = c;
                    goto post;
                }
            }
        }

        if (c == 'D' && !(v40 & 8) &&
            (Phone_Attr(ch_prevv | 0x100) & 2) &&
            (Phone_Attr(ch_nextv | 0x100) & 2) &&
            !(nxt_saved->flags & 0x20u)) {
            if (ch_next != 'A' || nxt_saved->next->value != 'T') {
                n->value = 't';
                n->b18 = c;
                goto post;
            }
        }
    }

t_after_u:
    if (c == 'T' && ch_next == 'u' && (ch_prev == '&' || ch_prev == '%') &&
        self->s1_word_start->b15 == 0xa &&
        (Phone_Attr(ch_prevv | 0x100) & 2)) {
        int32_t ok = 0;

        if (prv_saved != NULL && prv_saved->prev != NULL &&
            (prv_saved->prev->flags & 0x20u))
            ok = 1;
        if (!ok && (Phone_Attr(ch_prevv | 0x200) & 1))
            ok = 1;
        if (ok) {
            p = nxt_saved->next->next;
            if (p != NULL && !(attr_hi(p->value) & 0x40) &&
                !(attr_lo(p->value) & 8)) {
                n->value = 't';
                n->b18 = c;
                goto post;
            }
        }
    }

c_is_x:
    if (c == 'x' && (ch_prevv == 'T' || ch_prevv == 'D') &&
        st->rate_index > 0x13)
        n->value = 'D';

    if ((Phone_Attr(ch_next | 0x80) & 1) && st->rate_index < 0x13 &&
        !(Phone_Attr((int16_t)(ch16 | 0x80)) & 8)) {
        ret = Engine_NodeAlloc(self, n, 1, 3, 'p');
        goto post;
    }
    goto maybe_y;

drop_stop:
    ret = Engine_NodeFree(self, n, 0);
    did_next = 1;
    goto post;

    /* ================= vowels ========================================== */
vowel_branch:
    ix = (int32_t)(int16_t)(ch16 | 0x100);
    if ((Phone_Attr(ix) & 2) && c != 'U' && (ch_prev == '&' || ch_prev == '%') &&
        st->pitch != 0) {
        if (ch_prevv == (int32_t)ch16 &&
            (Phone_Attr((int16_t)(ch16 | 0x180)) & 0x40) &&
            self->s1_word_start->b14 != 1 &&
            self->s1_next_start->b14 != 1) {
            ret = Engine_NodeAlloc(self, n, 0, 3, 'Y');
            ret->arg = 4;
            goto post;
        }
        if (ch_prevv == 0 || ch_prevv == (int32_t)ch16) {
            if (self->s1_word_start->b14 != 1 &&
                self->s1_next_start->b14 != 1) {
                if ((ch_prevv == 0 && (n->flags & 0x18u) && (Phone_Attr(ix) & 0x20) &&
                     c != 'a') ||
                    ch_prevv == (int32_t)ch16)
                    n->flags |= 0x40u;
                goto after_accent;
            }
        }
        /* L061a50 */
        f = n->flags;
        if ((f & 0x18u) && ch_prevv == '@') {
            n->flags = f | 0x40u;
            goto after_accent;
        }
        v40 = (uint8_t)((f >> 3) & 3);
        if (v40 != 0) {
            int32_t rate = st->rate_index;

            if ((Phone_Attr(ch_prevv | 0x100) & 2) && rate < 0xd) {
                n->flags = f | 0x40u;
                goto after_accent;
            }
            if (v40 == 2 && rate < 0xd && (Phone_Attr(ch_prevv) & 1)) {
                n->flags = f | 0x40u;
                goto after_accent;
            }
            if (rate <= 6 && (Phone_Attr(ch_prevv) & 0x42)) {
                n->flags = f | 0x40u;
                goto after_accent;
            }
        }
    }

after_accent:
    if (c == 'e' && !(n->flags & 0x20u)) {
        int32_t hit = 0;

        if (ch_next == 'K' && nxt2->value == 'S')
            hit = 1;
        else if (ch_next == 'G' && nxt2->value == 'Z')
            hit = 1;
        if (hit && (ch_prev == '&' || ch_prev == '%' || ch_prev == ' ')) {
            n->value = '|';
            goto post;
        }
    }

    if (c == '|' && !(n->flags & 0x20u) && ch_prev == 'B' &&
        (ch_nextv == 'H' || ch_nextv == 'd') &&
        ch_next != '&' && ch_next != '%') {
        n->value = 'E';
        goto post;
    }

    if (c == 'Y') {
        if ((ch_prev == '&' || ch_prev == '%') && ch_prevv == 'z' &&
            self->s1_word_start->b15 == 1) {
            ret = Engine_NodeFree(self, n, 0);
            goto post;
        }
        if ((ch_prev == '&' || ch_prev == '%') && ch_prevv == 's') {
            p = st->ctl->prev;
            p = p->prev;
            p = p->prev;
            if (p != NULL && p->value == 'C' &&
                self->s1_word_start->b15 == 1) {
                ret = Engine_NodeFree(self, n, 0);
                goto post;
            }
        }
    }

    if (c == 'l' && (ch_next == '[' || ch_next == 'D')) {
        n->value = 'j';
        ret = Engine_NodeAlloc(self, n, 0, 3, '@');
        ret->arg = 4;
        goto post;
    }

    if (c == 'm' && ch_next != '.' && ch_next != '?' && ch_next != ',') {
        n->value = 'M';
        ret = Engine_NodeAlloc(self, n, 0, 3, '@');
        goto post;
    }

    if (c == 'n' && ch_prev != 'q' && (Phone_Attr(ch_nextv | 0x100) & 2)) {
        n->value = 'N';
        ret = Engine_NodeAlloc(self, n, 0, 3, '@');
        goto post;
    }

    {
        uint8_t pv2 = (uint8_t)(Phone_Attr(ch_prevv | 0x100) & 2);

        if (pv2 != 0 && prv_saved != NULL && prv_saved->prev != NULL &&
            (prv_saved->prev->flags & 0x20u) && (n->flags & 0x20u) &&
            st->pitch != 0 && (Phone_Attr(ix) & 2) &&
            (ch_prev == '&' || ch_prev == '%')) {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            ret->arg = 5;
            goto post;
        }
        if ((ch_prevv == '@' || ch_prevv == 'o' || ch_prevv == 'v') &&
            (n->flags & 0x20u) && st->pitch != 0) {
            if (!(Phone_Attr(ix) & 2))
                goto no_pause;
            if (ch_prev == '&' || ch_prev == '%') {
                ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
                ret->arg = 5;
                n->flags |= 0x40u;
                goto post;
            }
        }
        if ((Phone_Attr(ix) & 2) && (n->flags & 0x20u) &&
            (ch_prev == '&' || ch_prev == '%') && pv2 != 0 &&
            !(Phone_Attr(ch_prevv | 0x200) & 2) && ch_prevv != '3' &&
            self->s1_word_start->b14 != 1 &&
            self->s1_next_start->b14 != 1) {
            ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
            ret->arg = 3;
            n->flags |= 0x40u;
            goto post;
        }
    }

no_pause:
    if (c == 'i' && !(n->flags & 0x38u)) {
        uint32_t cls = self->s1_word_start->b15;

        if (cls != 2 && cls != 5 && cls != 9 && cls != 0xc && cls != 0xd &&
            cls != 0xe && cls != 1 && cls != 3 && cls != 0xa && cls != 4 &&
            n->prev != NULL && n->prev->prev != NULL && n->next != NULL &&
            n->next->next != NULL) {
            t = n->next->next->next;
            if (t != NULL &&
                !(attr_hi(n->prev->prev->value) & 0x40) &&
                n->prev->value != ' ' && at_start == 0) {
                d = n->next->value;
                if (d != '.' && d != '?') {
                    d = n->next->next->value;
                    if (d != '.' && d != '?') {
                        d = t->value;
                        if (d != '.' && d != '?') {
                            c = '|';
                            n->value = c;
                        }
                    }
                }
            }
        }
    }

    if ((c == '@' || c == '|') && ch_next == 'L' &&
        !(Phone_Attr(ch_prev) & 0x10)) {
        int32_t dark = 0;

        if (nxt2 != NULL) {
            if (nxt2->next->value == 'L')
                dark = 1;
            else if (!(Phone_Attr(ch_next2 | 0x200) & 0x40) &&
                     ch_next2 != '%' && ch_next2 != '&' && ch_next2 != '[' &&
                     !(Phone_Attr(ch_next2 | 0x80) & 1)) {
                if (ch_next == 'L' && nxt2->value == 'Z')
                    dark = 1;
            } else {
                dark = 2;
            }
            if (dark == 1 && ch_next == 'L' && nxt2->value != 'Z')
                dark = 0;
        }
        if (dark == 0 && nxt2 == NULL)
            dark = 2;
        if (dark != 0) {
            n->value = 'l';
            ret = Engine_NodeFree(self, nxt_saved, 0);
            goto post;
        }
    }

    {
        uint8_t hi = (uint8_t)Phone_Attr(ch_prev | 0x80);

        if (((hi & 0x20) && !(Phone_Attr(ch_prev) & 2)) || (hi & 1)) {
            if ((Phone_Attr((int16_t)(ch16 | 0x80)) & 0x10) &&
                ch_next == 'N' && ch_prevv == 'q') {
                n->value = 'n';
                ret = Engine_NodeFree(self, nxt_saved, 0);
                goto post;
            }
        }
    }

    if (c == 'E' && ch_next == '@' && ch_prev == 'j' && !(n->flags & 0x18u)) {
        c = 'Y';
        n->value = c;
    }

    if ((Phone_Attr(c) & 1) && ch_next == 'R') {
        v40 = 0;
        if (nxt2 != NULL && (nxt2->flags & 0x18u))
            goto r_done;
        if (n->prev->value == 'F' && n->next->next != NULL &&
            ((c == '|' && n->next->next->value == '|') ||
             (c == '@' && n->next->next->value == '@'))) {
            Engine_NodeFree(self, n, 0);
            goto post;
        }
        if (c == 'v' || c == '|' || c == '@' || c == 'i') {
            n->value = '3';
        } else if (c == 'E') {
            n->value = '4';
        } else if (c == 'I' && ch_next2 != '|' && ch_next2 != '@' &&
                   ch_next2 != 'O' &&
                   !(ch_next2 == '[' &&
                     (nxt2->next->value == '|' || nxt2->next->value == '@' ||
                      nxt2->next->value == 'O'))) {
            n->value = '5';
        } else if (c == 'a' || c == 'e' || c == 'A') {
            n->value = 'k';
        } else if (c == 'o' && ch_next2 != 'E' && ch_next2 != 'i' &&
                   ch_next2 != '|' && ch_next2 != 'e') {
            n->value = 'r';
        } else if (c == 'O' || c == 'w') {
            n->value = 'g';
        } else if (c == 'b' || c == 'u') {
            n->value = 'c';
        } else if ((c == 'I' && ch_next == 'R' &&
                    nxt2->value != '@' && nxt2->value != 'O' &&
                    nxt2->value != '|') ||
                   c == 'f' || c == 'y') {
            nxt_saved->value = '3';
        }

        if (n->value != c) {
            int32_t kill = 0;

            if (Phone_IsVowel(nxt2->value))
                kill = 1;
            else {
                p = n->next->next;
                if (p->value == '[' && Phone_IsVowel(p->next->value))
                    kill = 1;
            }
            if (kill)
                ret = Engine_NodeFree(self, nxt_saved, 0);
        }
        d = nxt2->value;
        if (!(attr_x(d) & 2)) {
            if (d == '[' && (attr_x(nxt2->next->value) & 2))
                goto post;
            if (nxt_saved->value != '3')
                ret = Engine_NodeFree(self, nxt_saved, 0);
        }
    }

r_done:
    if (n->value == '3' && n->prev->value == 'I' &&
        (n->prev->flags & 0x20u)) {
        d = n->next->value;
        if (d != '|' && d != '@' && d != 'O' &&
            !(d == '[' && (n->next->next->value == '|' ||
                           n->next->next->value == '@' ||
                           n->next->next->value == 'O'))) {
            n->prev->value = '5';
            n = Engine_NodeFree(self, n, 0);
            ret = n;
            goto post;
        }
    }

    if ((c == '3' || c == '4' || c == '5' || c == 'k' || c == 'r' ||
         c == 'c' || c == 'g') && ch_next == 'R') {
        d = nxt2->value;
        if (d == 'E' || d == 'A' || d == 'e' || d == 'a' || d == 'I' ||
            d == 'i' || d == 'f' || d == 'v' || d == 'O' || d == 'y' ||
            d == '@' || d == 'b' || d == '|')
            ret = Engine_NodeFree(self, nxt_saved, 0);
    }

    if ((c == '3' && ch_prevv == '3' && ch_prev != '&') ||
        (ch_prevv == '3' && v40 == '3')) {
        Engine_NodeAlloc(self, n, 0, 3, 'R');
        goto post;
    }

    d = n->value;
    if ((Phone_Attr((int16_t)((int16_t)(int8_t)d | 0x200)) & 2) || d == '3') {
        if (ch_prev == '%' || ch_prev == '&') {
            if (prv_saved != NULL && prv_saved->prev != NULL) {
                p = prv_saved->prev;
                d = p->value;
                if ((Phone_Attr((int16_t)((int16_t)(int8_t)d | 0x200)) & 2) ||
                    d == '3') {
                    p = p->prev;
                    if (p == NULL)
                        goto post;
                    while (p != NULL) {
                        d = p->value;
                        if ((attr_lo(d) & 0x80) && d != ' ')
                            break;
                        p = Node_PrevBoundary(self, p);
                    }
                    if (p == NULL)
                        goto post;
                    d = p->value;
                    if (!((Phone_Attr((int16_t)((int16_t)(int8_t)d | 0x200)) & 2) ||
                          d == '3')) {
                        ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
                        ret->arg = 6;
                        goto post;
                    }
                }
            }
        }
    }

    if (n->value == 'b' && ch_prev == 'Y' && (n->flags & 0x18u) &&
        prv_saved != NULL && prv_saved->prev != NULL) {
        d = prv_saved->prev->value;
        if ((attr_hi(d) & 0x20) && !(d != '&' && d == '%')) {
            n->value = 'U';
            ret = Engine_NodeFree(self, prv_saved, 0);
            goto post;
        }
    }

maybe_y:
    if (attr_x(c) & 2) {
        int32_t ins = 0;

        if (Phone_Attr(ch_prev | 0x180) & 0x40)
            ins = 1;
        else if (ch_prev == '[') {
            p = prv_saved->prev;
            if (Phone_Attr((int16_t)((int16_t)(int8_t)p->value | 0x180)) & 0x40)
                ins = 1;
        }
        if (ins) {
            ret = Engine_NodeAlloc(self, n, 0, 3, 'Y');
            ret->arg = 3;
        }
    }

    /* ================= shared clean-up ================================= */
post:
    if ((ch_prevv == '@' || ch_prevv == '|') &&
        (n->value == '@' || n->value == '|') &&
        (ch_prev == '&' || ch_prev == '%')) {
        ret = Engine_NodeAlloc(self, n, 0, 3, ' ');
        ret->arg = 4;
        n->flags |= 0x40u;
    }

    if (n->value == 'l' && n->next->value == '@') {
        n->value = 'j';
        ret = Engine_NodeAlloc(self, n, 0, 3, '@');
    }

    if (n->value == 'l') {
        d = n->prev->value;
        if (d == 'T' || d == 'F' || d == 'K' || d == 'R' || d == 'S' ||
            d == 's' || d == 'X' ||
            (d == 'W' && n->prev->prev->value == 'K') ||
            (d == '[' && n->prev->prev->value == 'K')) {
            n->value = '@';
            n->arg = 4;
            ret = Engine_NodeAlloc(self, n, 1, 3, 'j');
        }
    }

    if (n->value == 'l') {
        d = n->prev->value;
        if (d == 'G' || (d == 'D' && n->prev->prev->value == 'N') ||
            d == 'P' || d == 'Z' || d == 'B') {
            n->value = '|';
            n->arg = 4;
            ret = Engine_NodeAlloc(self, n, 1, 3, 'j');
        }
    }

    if (n->value == 'j' && n->prev->value == '@' &&
        n->prev->prev->value == 't' &&
        (ch_next == '.' || ch_next == '&' || ch_next == '%' ||
         ch_next == ' ' || ch_next == ',' || ch_next == '?')) {
        n->value = 'l';
        ret = Engine_NodeFree(self, n->prev, 0);
    }

    if (n->value == '~') {
        d = n->next->value;
        if ((attr_hi(d) & 0x40) || d == ' ')
            ret = Engine_NodeAlloc(self, n, 1, 3, 'p');
    }

    if (n->value == 'l' && n->prev->value == '3' &&
        n->prev->prev->value == 't')
        ret = Engine_NodeAlloc(self, n, 0, 3, 'R');

    if (n->value == 'l' && n->prev->value == '3') {
        d = n->next->value;
        if (d == ' ' || (attr_lo(d) & 8) || (attr_hi(d) & 0x40)) {
            ret = Engine_NodeAlloc(self, n, 0, 3, 'R');
            ret->arg = 5;
        }
    }

    if ((attr_x(n->value) & 2) && ch_prevv == '4' && prv_saved != NULL) {
        d = prv_saved->value;
        ret = Engine_NodeAlloc(self, prv_saved,
                               (d == '&' || d == '%') ? 0 : 1, 3, 'R');
    }

    if ((attr_x(n->value) & 2) && ch_prevv == '5' && prv_saved != NULL) {
        d = prv_saved->value;
        ret = Engine_NodeAlloc(self, prv_saved,
                               (d == '&' || d == '%') ? 0 : 1, 3, 'R');
    }

    if ((attr_x(n->value) & 2) && ch_prev == 'g' && prv_saved != NULL) {
        ret = Engine_NodeAlloc(self, prv_saved, 1, 3, 'R');
        ret->arg = 3;
    }

    if (n->value == 'l' && n->prev->value == 'Y')
        ret = Engine_NodeFree(self, n->prev, 0);

    if (ret == NULL)
        ret = n;

    if (did_next == 1 && (ch_next == '&' || ch_next == '%'))
        nxt_saved->flags |= 0x80u;
    else if (did_prev == 1 && (ch_prev == '&' || ch_prev == '%'))
        prv_saved->flags |= 0x80u;

    return ret;
}

/* Is the word after "the press" one of the handful that make it a compound
 * ("press AT", "press conFERence", ...)?  The first phoneme of the following
 * word picks a group, then the rest of the group's phonemes must match.
 *
 * Note: the corpus never reaches this, so it is decompiled from the
 * disassembly but not verified against the original. */
/* @0x10064200 */
uint8_t TV_THISCALL Stage1_VowelAux(Engine *self)
{
    Node *n = self->s1_word_start->next;
    Node *p = n->next;
    int32_t k = (int8_t)n->value - 'A';
    uint8_t c;

    if ((uint32_t)k > 0x19)
        return 0;
    switch ((char)('A' + k)) {
    case 'A':
        return (uint8_t)(p->value == 'T');
    case 'F':
        c = p->value;
        if (c == 'w' && p->next->value == 'R')
            return 1;
        return (uint8_t)(c == 'I' && p->next->value == 'V');
    case 'N':
        return (uint8_t)(p->value == 'I' && p->next->value == 'N');
    case 'P':
        return (uint8_t)(p->value == 'f' && p->next->value == 'N' &&
                         p->next->next->value == 'D');
    case 'S':
        c = p->value;
        if (c == 'i' && p->next->value == 'K' && p->next->next->value == 'S')
            return 1;
        if (c == 'e' && p->next->value == 'V' && p->next->next->value == '|' &&
            p->next->next->next->value == 'N')
            return 1;
        return (uint8_t)(c == 'T' && p->next->value == 'o' &&
                         p->next->next->value == 'R');
    case 'T':
        return (uint8_t)(p->value == 'b');
    case 'W':
        return (uint8_t)(p->value == 'v' && p->next->value == 'N');
    case 'X':
        return (uint8_t)(p->value == 'R' && p->next->value == 'E');
    case 'Z':
        return (uint8_t)(p->value == '4' && p->next->value == 'R' &&
                         p->next->next->value == 'O');
    default:
        return 0;
    }
}
