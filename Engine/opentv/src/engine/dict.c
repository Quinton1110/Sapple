/*
 * The built-in pronunciation dictionary.
 *
 * The dictionary is a bit-packed blob inside the DLL.  The first letter of a
 * word selects a bucket, the remaining letters are packed into a key with a
 * small state machine (g_dict_key), and the bucket is scanned linearly for an
 * entry whose key bytes match.  The entry's payload is a second bit-packed
 * stream that unpacks, with a second state machine (g_dict_ph), into the
 * phoneme symbols and the stress marks between them.
 *
 * Homographs are stored with a "class 6" marker; which reading is taken then
 * depends on the word before, on the suffix the affix stripper removed, and
 * on a couple of hard-coded cases.
 */
#include "engine.h"
#include "crt.h"

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

/* Words whose pronunciation depends on the part of speech. */
/* @0x10095130 */ extern const char g_dict_homographs[];
/* @0x100f9adc */ extern const char g_dict_word_read[];    /* "#READ#"    */
/* @0x100f9ad0 */ extern const char g_dict_word_subject[]; /* "#SUBJECT#" */
/* Suffixes the affix stripper may have taken off. */
/* @0x100f9af0 */ extern const char g_dict_sfx_ing[];
/* @0x100f9aec */ extern const char g_dict_sfx_ed[];
/* @0x100f9ae8 */ extern const char g_dict_sfx_d[];
/* @0x100bf4c0 */ extern const char g_dict_sfx_s[];
/* @0x100f9ae4 */ extern const char g_dict_sfx_es[];

/* Read one byte of the dictionary blob. */
/* @0x10050d20 */
uint8_t TV_CDECL Dict_Byte(const void *p)
{
    return *(const uint8_t *)p;
}

/* The dictionary is a graph of stored references rather than C pointers,
 * because that is how the original walked it: it does its own arithmetic on
 * them, so they stay four-byte references here too.  DICT_ADDR reads the one
 * a reference points at; DICT_BASE is the bucket table. */
#define DICT_ADDR(r)  (*TV_REF(tv_ref, (r)))
#define DICT_BASE     TV_REF(tv_ref, g_dict_base)
#define DICT_STRIDE   ((tv_ref)sizeof(tv_ref))

/* Walk the word between st->d14 and st->d18 through the dictionary.  On a hit
 * the letters are replaced by the phoneme nodes the entry unpacks to, and 1
 * is returned. */
/* @0x10050d30 */
uint8_t TV_THISCALL Lexicon_Try(Engine *self)
{
    StageCtx *st = &self->stage_ctx[1];
    Node **pd14 = &st->d14;
    Node **pd18 = &st->d18;
    Node *n, *cur, *nodeC = NULL;
    Node *stressed = NULL, *fallback = NULL;
    uint8_t key[32];
    char suffix[48];
    char word[112];
    uint8_t *bufp, *cmpp;
    tv_ref p, q, endp, cand = 0, lim, slot;
    int32_t letter, nletters = 0, nsuffix = 0, keylen, i, j;
    int32_t keep_start = 0, keep_end = 0, first_try = 1;
    int32_t flagA = 0, flagB = 0, wcls, wflags;
    uint8_t state = 2, pstate, shift, mask_last = 0, wclass = 0;
    uint8_t flag13 = 0, any_stress = 0, pend_stress = 0;
    uint8_t c, a;
    uint32_t v;

    if (self->s1_next_end != *pd18)
        keep_end = 1;
    if (self->s1_letters != *pd14)
        keep_start = 1;

    /* Pack every letter after the first into the lookup key. */
    key[0] = 0;
    bufp = key;
    n = (*pd14)->next;
    if ((*pd18)->next != n) {
        do {
            if (nletters >= 20)
                return 0;
            state = g_dict_key[state].next;
            *bufp |= (uint8_t)((g_dict_key[state].mask & n->value)
                               << g_dict_key[state].shift);
            if (state != 0) {
                bufp++;
                *bufp = (uint8_t)(g_dict_key[state].mask2 & n->value);
            }
            n = n->next;
            nletters++;
        } while ((*pd18)->next != n);
    }
    if (state == 2)
        bufp--;

    /* The first letter picks the bucket. */
    c = (*pd14)->value;
    if ((int8_t)c < 'A')
        return 0;
    letter = (int8_t)(c - 'A');
    lim = DICT_BASE[26];
    slot = DICT_BASE[letter] + (tv_ref)nletters * DICT_STRIDE;
    if (slot >= lim)
        return 0;
    p = DICT_ADDR(slot);
    endp = DICT_ADDR(slot + DICT_STRIDE);
    if (endp == p || DICT_ADDR(DICT_BASE[letter + 1]) <= p ||
        DICT_ADDR(lim) <= p)
        goto use_candidate;

    for (;;) {
        /* --- try the entry at p ----------------------------------------- */
        nsuffix = Dict_Byte(TV_REF_AT(p)) & 0xf;
        if (nletters != 0) {
            q = p + 1;
            cmpp = key;
            if (bufp > key) {
                do {
                    if (Dict_Byte(TV_REF_AT(q)) != *cmpp)
                        break;
                    q++;
                    cmpp++;
                } while (cmpp < bufp);
            }
            mask_last = g_dict_key[state].last_mask;
        } else {
            bufp = key;
            cmpp = key;
            mask_last = 0;
            state = 2;
            q = p;
        }

        if (cmpp == bufp &&
            (uint8_t)(Dict_Byte(TV_REF_AT(q)) & mask_last) ==
                (uint8_t)(*cmpp & mask_last)) {
            /* The key matched; the high nibble is the word class. */
            wclass = (uint8_t)((Dict_Byte(TV_REF_AT(p)) >> 4) & 0xf);
            n = self->s1_next_start;
            if ((n->flags & 0x20u) && n->arg == 4) {
                /* Keep this one in reserve and look for a better entry. */
                n->flags &= ~0x20u;
                cand = p;
                goto next_entry;
            }
            if (wclass != 6)
                goto accept;

            /* Homograph: work out which reading the context wants. */
            flagA = 0;
            flagB = 0;
            nodeC = NULL;
            n = self->s1_word_start;
            if (n != NULL) {
                n = n->prev;
                i = 0;
                while (n != NULL && i < 8) {
                    c = n->value;
                    if (Phone_Attr((int16_t)((int16_t)(int8_t)c | 0x80)) & 0x40)
                        break;
                    if (c == '&' || c == '%') {
                        nodeC = n;
                        break;
                    }
                    n = n->prev;
                    i++;
                }
            }

            word[0] = '#';
            i = 1;
            n = *pd14;
            if ((*pd18)->next != n) {
                while (i < 0x62) {
                    word[i] = (char)n->value;
                    i++;
                    n = n->next;
                    if ((*pd18)->next == n)
                        break;
                }
            }
            word[i] = '#';
            i++;
            word[i] = '\0';

            if (keep_end != 0) {
                j = 0;
                if (self->s1_next_end->next != n) {
                    while (j < 0x1e) {
                        c = n->value;
                        if (c != '[') {
                            suffix[j] = (char)c;
                            j++;
                        }
                        n = n->next;
                        if (self->s1_next_end->next == n)
                            break;
                    }
                }
                suffix[j] = '\0';
                if (tv_strcmp(suffix, g_dict_sfx_ing) == 0 ||
                    tv_strcmp(suffix, g_dict_sfx_ed) == 0 ||
                    tv_strcmp(suffix, g_dict_sfx_d) == 0)
                    flagA = 1;
                else if (tv_strcmp(suffix, g_dict_sfx_s) == 0 ||
                         tv_strcmp(suffix, g_dict_sfx_es) == 0)
                    flagB = 1;
            } else if (tv_strcmp(word, g_dict_word_read) == 0 &&
                       self->s1_word_start != NULL && first_try != 0) {
                /* "read": the tense comes from the auxiliary in front. */
                first_try = 0;
                wflags = (int32_t)self->s1_word_start->flags;
                wcls = wflags & 0xf00;
                if (wcls == 0xc00 || wcls == 0xd00)
                    goto next_entry;
                if (wcls == 0x200 && (wflags & 0xf000) == 0x1000)
                    goto next_entry;
            }

            if (flagA == 0 && tv_strstr(g_dict_homographs, word) != NULL) {
                if (tv_strcmp(word, g_dict_word_subject) == 0) {
                    Node *t = self->s1_next_end->next;

                    if (t != NULL && NODE_TYPE(t) == 1 && t->value == ':')
                        goto next_entry;
                }
                if (self->s1_word_start == NULL)
                    goto accept;
                wflags = (int32_t)self->s1_word_start->flags;
                wcls = wflags & 0xf00;
                if (wcls == 0x500 && (wflags & 0xf000) == 0x1000 && flagB == 0)
                    goto accept;
                if (wcls == 0x100)
                    goto next_entry;
                if (wcls == 0x300) {
                    if ((wflags & 0xf000) != 0x1000 || flagB != 0)
                        goto next_entry;
                    goto accept;
                }
                if (wcls == 0x500)
                    goto next_entry;
                if (wcls == 0x600) {
                    if ((wflags & 0xf000) != 0x1000 || flagB == 0)
                        goto next_entry;
                    goto accept;
                }
                if (wcls == 0x800 || wcls == 0xe00 || wcls == 0xc00)
                    goto next_entry;
                if (wcls == 0xf00) {
                    if ((wflags & 0xf000) != 0x1000 || flagB != 0)
                        goto next_entry;
                    goto accept;
                }
                if (wcls == 0x400) {
                    if ((wflags & 0xf000) == 0x2000)
                        goto next_entry;
                    if ((wflags & 0xf000) != 0x1000 || flagB != 0)
                        goto accept;
                    goto next_entry;
                }
                if (nodeC != NULL && (nodeC->flags & 0xf00u) == 0x100u)
                    goto next_entry;
            }
            goto accept;
        }

next_entry:
        keylen = (int32_t)(bufp - key);
        if (nsuffix != 0)
            p += (tv_ref)((int32_t)g_dict_skip[state * 17 + nsuffix] + keylen);
        else
            p += (tv_ref)(keylen + 3);
        if (endp > p)
            continue;
use_candidate:
        if (cand == 0)
            return 0;
        p = cand;
    }

accept:
    if (wclass != 0 && self->s1_1c2a == 0) {
        if (keep_start == 0 && keep_end == 0 && wclass != 6 && wclass != 0xf)
            self->s1_next_start->value = '%';
        if (self->s1_1c2c == 0)
            self->s1_1c2c = wclass;
    }

    if (nsuffix == 0) {
        q++;
        self->s1_1c29 = Dict_Byte(TV_REF_AT(q));
        return 1;
    }

    /* Drop the letters and unpack the phonemes in their place. */
    cur = *pd14;
    if (*pd18 != cur) {
        do {
            cur = Engine_NodeFree(self, cur, 1);
        } while (*pd18 != cur);
    }
    cur = Engine_NodeFree(self, cur, 0);
    *pd14 = cur;
    if (state == 2)
        q++;

    pstate = g_dict_key[state].pstate;
    i = nsuffix;
    do {
        pstate = g_dict_ph[pstate].next;
        shift = g_dict_ph[pstate].shift;
        a = Dict_Byte(TV_REF_AT(q));
        v = ((uint32_t)(int32_t)(int8_t)a & g_dict_ph[pstate].mask) >> shift;
        if (pstate >= 1) {
            if (pstate <= 2) {
                q++;
                v |= (uint8_t)(Dict_Byte(TV_REF_AT(q)) &
                               g_dict_ph[pstate].mask2);
            } else if (pstate == 3) {
                q++;
            }
        }
        c = (uint8_t)(v | 0x40u);
        if (c == 0x60 || (uint8_t)(c & 0x3c) == 0x1c || c == 0x5b)
            c = (uint8_t)(c - 0x2b);

        if ((int8_t)c == '0') {
            flag13 = 1;
        } else if ((int8_t)c == '1') {
            any_stress = 1;
            if (self->s1_1c2a == 1)
                pend_stress = 3;
            else if (self->s1_1c2b == 1)
                pend_stress = 1;
            else
                pend_stress = 2;
        } else if ((int8_t)c == '2') {
            pend_stress = 1;
        } else {
            cur = Engine_NodeAlloc(self, cur, 1, 3, c);
            if (pend_stress != 0) {
                cur->flags = (cur->flags & ~0x18u) |
                             ((uint32_t)((int32_t)(int8_t)pend_stress << 3) & 0x18u);
            } else if (any_stress != 1 && (Phone_Attr((int8_t)cur->value) & 1) &&
                       stressed == NULL && flag13 == 0) {
                if (Phone_Attr((int16_t)((int16_t)(int8_t)cur->value | 0x200)) & 0x40) {
                    if (fallback == NULL)
                        fallback = cur;
                } else {
                    stressed = cur;
                }
            }
            flag13 = 0;
            pend_stress = 0;
        }
        i--;
    } while (i != 0);

    *pd18 = cur;
    n = (*pd14)->next;
    *pd14 = n;
    if (keep_start == 0)
        self->s1_letters = n;
    if (keep_end == 0)
        self->s1_next_end = cur;

    if (any_stress == 1)
        return 1;
    if (stressed == NULL) {
        stressed = fallback;
        if (stressed == NULL)
            return 1;
    }
    if (self->s1_1c2a == 1) {
        stressed->flags |= 0x18u;
        return 1;
    }
    if (self->s1_1c2b == 1) {
        stressed->flags = (stressed->flags & ~0x10u) | 8u;
        return 1;
    }
    stressed->flags = (stressed->flags & ~8u) | 0x10u;
    return 1;
}
