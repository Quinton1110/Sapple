/*
 * Letter-to-sound rules.
 *
 * When neither lexicon knows a word, its letters are walked from the end back
 * to the start.  For each letter the first rule whose feature, left-context
 * and right-context tests all pass replaces the letters it covers with that
 * rule's phonemes.  Stress is worked out as the phonemes appear: a syllable
 * opens at every stressable vowel, and the one that ends up carrying the word
 * stress is marked when the word (or the second pass over the tail that the
 * affix stripper left behind) finishes.
 */
#include "engine.h"

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

static uint8_t attr_lo(uint8_t c)
{
    return Phone_Attr((int8_t)c);
}

static uint8_t attr_hi(uint8_t c)
{
    return Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x80));
}

static uint8_t attr_x(uint8_t c)
{
    return Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x100));
}

static uint8_t attr_y(uint8_t c)
{
    return Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x200));
}

/* @0x1005f7b0 */
void TV_THISCALL Stage1_Rules(Engine *self)
{
    StageCtx *st = &self->stage_ctx[1];
    const LtsEntry *r;
    const uint8_t *p;
    Node *n, *q, *span_start;
    int32_t syl_count = 0, carry_bits = 0, stress_lvl = 2;
    int32_t feat_carry = 0, class_bits = 0, pending_syl = 0;
    int32_t bits_lo, bits_hi, sy;
    int8_t budget = 4, flags29;
    uint8_t first_syl = 1, done_flag = 0, has_more = 0, applied = 0;
    uint8_t at_end = 0, stressed = 0, prev_ch = 0, reduced = 0;
    uint8_t c;
    int16_t ch16;

    span_start = self->s1_next_start;
    self->lts_pending = NULL;
    self->lts_back = NULL;
    self->lts_cur = NULL;
    self->lts_second_pass = 0;
    if (self->s1_1c2a == 1)
        stress_lvl = 3;
    else if (self->s1_1c2b == 1)
        stress_lvl = 1;

    self->lts_syllables = 1;
    self->lts_feat_lo = 1;
    self->lts_feat_hi = 0;
    self->lts_reduce = 0;
    self->lts_cur = st->d18;
    if (self->s1_next_end != st->d18)
        has_more = 1;
    flags29 = (int8_t)self->s1_1c29;
    if (self->s1_1c2d != 0) {
        switch (self->s1_1c2d) {
        case 2: class_bits = 0x80; break;
        case 3: class_bits = 0x100; break;
        case 4: class_bits = 0x200; break;
        case 5: class_bits = 0x400; break;
        case 6: class_bits = 0x1000; break;
        case 7: class_bits = 0x800; break;
        default: break;
        }
        self->lts_feat_hi = (uint32_t)class_bits;
    }

    do {
        n = self->lts_cur;
        if (NODE_TYPE(n) != 2) {
            /* Not a letter: a morpheme boundary, or a phoneme already made. */
            at_end = 1;
            if (self->lts_second_pass == 1) {
                if (self->lts_pending != NULL)
                    Lts_Syllable(self, 0);
                return;
            }
            c = (uint8_t)((n->flags >> 3) & 3);
            if (c != 0) {
                reduced = 1;
                if (c != 1 || self->s1_1c2b != 0)
                    stressed = 1;
            } else if (attr_x(n->value) & 2) {
                reduced = 0;
            }
            self->lts_feat_lo = 0;
            q = Engine_StagePrev(self, n);
            if (q != NULL && q->value == '[') {
                n = self->lts_cur;
                if (attr_hi(n->value) & 0x20) {
                    self->lts_syllables++;
                } else {
                    self->lts_feat_lo = 2;
                    if (!(attr_y(n->value) & 0x40))
                        self->lts_feat_lo = 0x4002;
                }
                self->lts_feat_hi |= 4u;
            }
            self->lts_cur = q;
            goto tail;
        }

        c = n->value;
        if (c < '@' || c > '[') {
            self->lts_cur = Engine_NodeFree(self, n, 0);
            goto tail;
        }
        r = TV_REF(LtsEntry, g_lts_rules[(int8_t)c]);
        self->lts_back = n;
        for (;;) {
            if (Lts_TestFeatures(self, r) && Lts_MatchLeft(self, r) &&
                Lts_TestContext(self, TV_REF(uint8_t, r->cond), self->lts_back, 0))
                break;
            self->lts_back = self->lts_cur;
            r++;   /* 0x14 in the original, which is sizeof(LtsEntry) */
        }

        /* Replace the letters the rule covered with its phonemes. */
        while (self->lts_back != self->lts_cur)
            self->lts_back = Engine_NodeFree(self, self->lts_back, 1);
        self->lts_cur = Engine_NodeFree(self, self->lts_cur, 0);
        q = st->d14;
        if (NODE_TYPE(q) == 6)
            at_end = 1;
        self->lts_back = NULL;

        p = TV_REF(uint8_t, r->out);
        if (*p != 0 && (int8_t)*p < 0x20) {
            if (*p == 0x10) {
                /* drop the morpheme boundary, except after "MC" or "H[" */
                if (at_end == 0) {
                    Node *b = q->prev;
                    if (b->value == '[') {
                        Node *f = self->s1_letters;
                        c = f->value;
                        if (!(c == 'M' && f->next->value == 'C') &&
                            !(c == 'H' && f->next->value == '[')) {
                            Engine_NodeFree(self, b, 0);
                            st->d14 = self->s1_letters;
                        }
                    }
                }
                p++;
            }
            if ((int8_t)*p < 0x10) {
                self->lts_syllables = 0;
                first_syl = 0;
                pending_syl = (int8_t)*p;
                p++;
            }
        }

        c = *p;
        while (c != 0) {
            if (prev_ch != c || !(attr_hi(c) & 0x20)) {
                self->lts_back = Engine_NodeAlloc(self, self->lts_cur, 1, 3, c);
                if (NODE_TYPE(self->s1_next_end) == 6)
                    self->s1_next_end = self->lts_back;
            }
            ch16 = (int16_t)(int8_t)c;
            p++;
            if (attr_lo(c) & 1) {
                /* A stressable vowel opens a syllable. */
                if (first_syl != 0 && self->lts_syllables > 0)
                    self->lts_syllables--;
                if (TV_REF(uint32_t, r->set)[1] & 8)
                    self->lts_reduce = 0;
                if (self->lts_pending != NULL) {
                    Lts_Syllable(self, 0);
                    if (!(attr_lo(self->lts_back->value) & 1)) {
                        self->lts_reduce = 0;
                        syl_count++;
                        goto next_syllable;
                    }
                }
                applied = 0;
                if (self->s1_1c29 != 0 && done_flag == 0) {
                    int8_t left = budget;
                    budget--;
                    if (left > 0) {
                        int32_t kind = flags29 & 3;
                        applied = 1;
                        flags29 = (int8_t)(flags29 >> 2);
                        switch (kind) {
                        case 0:
                            reduced = 0;
                            self->lts_reduce = 1;
                            self->lts_pending = self->lts_back;
                            break;
                        case 1:
                            reduced = 0;
                            self->lts_reduce = 0;
                            break;
                        case 2:
                            reduced = 1;
                            self->lts_back->flags =
                                (self->lts_back->flags & ~0x10u) | 8u;
                            self->lts_reduce = 0;
                            break;
                        default:
                            stressed = 1;
                            reduced = 1;
                            self->lts_back->flags =
                                (self->lts_back->flags & ~0x18u) |
                                ((uint32_t)(stress_lvl << 3) & 0x18u);
                            self->lts_reduce = 0;
                            break;
                        }
                        goto after_stress;
                    }
                }
                if (stressed == 0) {
                    if (syl_count == 1 && self->lts_syllables > 1)
                        syl_count++;
                    syl_count += pending_syl;
                    pending_syl = 0;
                    syl_count++;
                    if (syl_count >= 3) {
                        self->lts_back->flags =
                            (self->lts_back->flags & ~0x18u) |
                            ((uint32_t)(stress_lvl << 3) & 0x18u);
                        stressed = 1;
                        reduced = 1;
                    }
                } else if (self->lts_second_pass == 0) {
                    if (reduced != 0) {
                        reduced = 0;
                    } else {
                        self->lts_back->flags =
                            (self->lts_back->flags & ~0x10u) | 8u;
                        reduced = 1;
                    }
                }
after_stress:
                if (applied != 0) {
                    applied = 0;
                    goto next_syllable;
                }
                if ((first_syl == 1 && self->lts_back->next != NULL &&
                     (attr_x(self->lts_back->next->value) & 0x80)) ||
                    (c == 'U' && self->lts_back->prev != NULL &&
                     (attr_lo(self->lts_back->prev->value) & 8))) {
                    self->lts_reduce = 0;
                    goto next_syllable;
                }
                if (reduced == 0 &&
                    (self->lts_syllables != 0 || c == 'o' || c == 'a') &&
                    !(Phone_Attr((int16_t)(ch16 | 0x200)) & 0x40) &&
                    !(TV_REF(uint32_t, r->set)[0] & 0x4000)) {
                    self->lts_reduce = 1;
                } else {
                    self->lts_reduce = 0;
                }
next_syllable:
                self->lts_syllables = 0;
                carry_bits = 0;
                first_syl = 0;
                self->lts_pending = self->lts_back;
            } else if (c != '[') {
                /* A consonant may still start a new syllable. */
                sy = self->lts_syllables;
                if ((sy > 0 && syl_count != 0) || sy > 1)
                    carry_bits = 0;
                if (!((class_bits & 0x400) && *p == 'T' && c == 'S') &&
                    *p != '[') {
                    int32_t skip = 0;
                    if (sy == 1) {
                        if (prev_ch == 'R' &&
                            (Phone_Attr((int16_t)(ch16 | 0x100)) & 1))
                            skip = 1;
                        else if ((c == 'K' || c == 'G') && prev_ch == 'W')
                            skip = 1;
                    }
                    if (!skip)
                        self->lts_syllables = sy + 1;
                }
            }
            if (c == '[') {
                if (attr_lo(prev_ch) & 0x80)
                    c = prev_ch;
                q = self->s1_next_start;
                if ((q->flags & 0x18) == 8) {
                    reduced = 1;
                    q->flags = q->flags & ~0x18u;
                }
            }
            prev_ch = c;
            c = *p;
        }

        /* The rule's output is done. */
        done_flag = at_end;
        if ((at_end == 1 || self->lts_second_pass == 1) &&
            self->lts_pending != NULL) {
            if (stressed == 0) {
                self->lts_pending->flags =
                    (self->lts_pending->flags & ~0x18u) |
                    ((uint32_t)(stress_lvl << 3) & 0x18u);
                stressed = 1;
                self->lts_reduce = 0;
                reduced = 1;
            }
            Lts_Syllable(self, 1);
            if (self->lts_feat_lo & 0x4000u)
                self->lts_reduce = 0;
        }

        bits_lo = (int32_t)(TV_REF(uint32_t, r->set)[0] & 0xffff7fffu);
        if (reduced == 1 && (attr_x(prev_ch) & 2))
            bits_lo |= 0x4000;
        bits_hi = (int32_t)(TV_REF(uint32_t, r->set)[1] & 0xffff7fffu);
        self->lts_feat_hi = (uint32_t)bits_hi;
        if (feat_carry != 0)
            self->lts_feat_hi = (uint32_t)(feat_carry | bits_hi);
        else
            feat_carry = bits_hi & 4;
        if (class_bits != 0)
            self->lts_feat_hi |= (uint32_t)class_bits;
        else
            class_bits = (int32_t)(self->lts_feat_hi & 0x1f80u);

        if (bits_lo != 0) {
            if (bits_lo == 1)
                self->lts_feat_lo |= 1u;
            else
                self->lts_feat_lo = (uint32_t)(carry_bits | bits_lo);
            carry_bits = bits_lo & 0x2048;
            if (carry_bits != 0 && syl_count != 0)
                self->lts_syllables = 0;
        }

        if (self->lts_cur->value == '[') {
            if (self->s1_1c29 != 0)
                self->lts_feat_hi |= 4u;
            else if (reduced == 1 || self->lts_reduce == 0)
                self->lts_feat_lo |= 0x4000u;
        }

tail:
        if (self->lts_cur == span_start && has_more == 1) {
            self->lts_feat_lo = 1;
            self->lts_cur = self->s1_next_end;
            if (class_bits != 0)
                self->lts_feat_hi |= (uint32_t)class_bits;
            else
                self->lts_feat_hi = 0;
            feat_carry = 0;
            self->lts_second_pass = 1;
            carry_bits = 0;
            syl_count = 0;
            self->lts_syllables = 1;
            prev_ch = 0;
            reduced = 0;
        }
    } while (self->lts_cur != span_start);
}

/* The schwa an unstressed syllable falls back to when none of the special
 * cases apply. */
static uint8_t lts_schwa(uint8_t pc, uint8_t nxt1)
{
    int16_t c1 = (int16_t)(int8_t)nxt1;

    if ((Phone_Attr((int16_t)(c1 | 0x180)) & 4) ||
        (Phone_Attr((int16_t)(c1 | 0x100)) & 0x80)) {
        if (!(Phone_Attr((int8_t)pc) & 8))
            return '|';
    }
    if ((Phone_Attr((int16_t)((int16_t)(int8_t)pc | 0x180)) & 4) && nxt1 == 'S')
        return '|';
    return '@';
}

/* Finish the syllable whose vowel is still pending: coalesce a "y" glide into
 * the consonant before it ("dune" -> "June", "tune" -> "chune"), and reduce
 * the vowel when nothing has given the syllable any stress. */
/* @0x100605c0 */
void TV_THISCALL Lts_Syllable(Engine *self, int32_t final)
{
    Node *v = self->lts_pending;
    Node *pv = v->prev;
    uint8_t pc = pv->value;    /* the consonant before the vowel */
    uint8_t vowel, nxt1 = 0, nxt2 = 0;
    uint8_t a, b, nx;
    int16_t ch16;
    Node *q;

    if (v->value == 'U') {
        v->value = 'b';
        ch16 = (int16_t)(int8_t)pc;
        a = Phone_Attr((int16_t)(ch16 | 0x100));
        b = Phone_Attr((int16_t)(ch16 | 0x180));
        if (!(a & 4) && !(b & 1)) {
            if (!(b & 4) && !(a & 2) && pc != 'X') {
                Engine_NodeAlloc(self, self->lts_pending, 0, 3, 'Y');
                v = self->lts_pending;
                q = v->prev;
                q->flags = ((v->flags ^ q->flags) & 0x20u) ^ q->flags;
            }
        } else if (pc == 'S' && self->lts_pending->next != NULL) {
            nx = self->lts_pending->next->value;
            if (nx == 'R')
                pv->value = 's';
            else if (nx != 'L' && final == 0)
                pv->value = 's';
        } else {
            q = self->lts_pending;
            if (!(q->flags & 0x18) && final == 0) {
                int32_t yod;

                nx = q->next->value;
                if ((Phone_Attr((int8_t)nx) & 8) ||
                    (Phone_Attr((int16_t)((int16_t)(int8_t)nx | 0x80)) & 0x40) ||
                    nx == ' ')
                    yod = (pc == 'N');
                else
                    yod = 1;
                if (yod && (uint32_t)((int32_t)ch16 - 'D') <= 0x16) {
                    switch ((int32_t)ch16 - 'D') {
                    case 0:  pv->value = 'J'; break;   /* DU  -> J  */
                    case 10: Engine_NodeAlloc(self, q, 0, 3, 'Y'); break; /* NU */
                    case 16: pv->value = 'C'; break;   /* TU  -> CH */
                    case 22: pv->value = 'z'; break;   /* ZU  -> ZH */
                    default: break;
                    }
                }
            }
        }
        /* only the "U" branch runs the H test: the original jumps straight
         * past it when the pending vowel is anything else */
        if (pc == 'H')
            self->lts_reduce = 0;
    }
    if (self->lts_reduce != 1)
        goto done;

    vowel = self->lts_pending->value;
    if (!Lts_TestContext(self, g_lts_cond_strong, pv, 0) && pc == 'E' &&
        pv->prev->value == 'L' && vowel != 'v' && vowel != 'a' && vowel != 'e')
        pv->value = 'Y';

    q = Engine_StageNext(self, self->lts_pending);
    if (q != NULL) {
        nxt1 = q->value;
        q = Engine_StageNext(self, q);
        if (q != NULL)
            nxt2 = q->value;
    }

    if (!Lts_TestContext(self, g_lts_cond_strong, self->lts_pending, 0) &&
        vowel == 'O' &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)nxt1 | 0x80)) & 0x20) &&
        ((Phone_Attr((int8_t)nxt2) & 8) || nxt2 == 0) &&
        self->s1_1c29 == 0)
        goto done;

    if (Lts_TestContext(self, g_lts_cond_strong, self->lts_pending, 0) &&
        self->lts_second_pass == 0 && self->s1_1c29 == 0 &&
        (Phone_Attr((int16_t)((int16_t)(int8_t)nxt1 | 0x80)) & 0x20)) {
        /* An open syllable before another one keeps its vowel. */
        if (!(Phone_Attr((int16_t)((int16_t)(int8_t)nxt2 | 0x80)) & 0x20) ||
            ((Phone_Attr((int8_t)nxt1) & 0x20) && nxt2 == 'R') ||
            (nxt1 == 'K' && nxt2 == 'W') ||
            nxt1 == nxt2 ||
            ((Phone_Attr((int8_t)pv->prev->value) & 8) && pc == 'K' &&
             vowel == 'o' && nxt1 == 'N')) {
            if (vowel != 'b' && (vowel != 'O' || nxt2 == '['))
                goto reduce;
        }
        self->lts_feat_lo |= 0x4000u;
        goto done;
    }

reduce:
    ch16 = (int16_t)(int8_t)vowel;
    if (Phone_Attr((int16_t)(ch16 | 0x200)) & 0x40)
        goto done;
    {
        int32_t k = (int32_t)ch16 - 'E';
        uint8_t rep;

        switch ((uint32_t)k <= 0x2d ? k : -1) {
        case 0:  /* E */
        case 4:  /* I */
        case 36: /* i */
            rep = '|';
            break;
        case 32: /* e */
            rep = (nxt1 == 'L' || nxt1 == 'R') ? '@' : '|';
            break;
        case 34: /* g */
        case 38: /* k */
        case 45: /* r */
            rep = '3';
            break;
        default:
            rep = lts_schwa(pc, nxt1);
            break;
        }
        self->lts_pending->value = rep;
    }

done:
    self->lts_pending = NULL;
}
