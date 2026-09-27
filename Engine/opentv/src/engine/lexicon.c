/*
 * The lexicon and the function-word classifier.
 */
#include "engine.h"
#include "crt.h"

/* The closed-class word lists, each a '#'-separated set the classifier
 * searches with strstr.  The word handed in is framed with '#' too, so a hit
 * is an exact match. */
/* @0x100954e0 */ extern const char g_words_article[];     /* A AN THE */
/* @0x100954f0 */ extern const char g_words_pronoun[];     /* HE SHE IT */
/* @0x10095500 */ extern const char g_words_quant[];       /* SUCH OTHER ... */
/* @0x10095540 */ extern const char g_words_quant_pl[];    /* FEW MANY ALL ... */
/* @0x10095560 */ extern const char g_words_conj[];        /* AND OR YET ... */
/* @0x10095650 */ extern const char g_words_prep[];        /* DESPITE BETWEEN ... */
/* @0x10095738 */ extern const char g_words_number[];      /* ZERO ONE TWO ... */
/* @0x100957c0 */ extern const char g_words_poss[];        /* OUR HIS HER ... */
/* @0x10095808 */ extern const char g_words_be[];          /* BE AM ARE ... */
/* @0x10095830 */ extern const char g_words_have[];        /* HAVE HAS HAD ... */
/* @0x10095848 */ extern const char g_words_demon[];       /* THIS THESE THOSE */
/* The few words inside those lists that are treated differently again. */
/* @0x10095884 */ extern const char g_word_that[];
/* @0x1009587c */ extern const char g_word_which[];
/* @0x10095874 */ extern const char g_word_to[];
/* @0x1009586c */ extern const char g_word_one[];
/* @0x10095864 */ extern const char g_word_this[];
/* @0x1009585c */ extern const char g_word_lets[];

/* Classify a closed-class ("function") word.  Bits 8-11 of the node's flags
 * hold the class, bits 12-15 how the prosody should treat it.  Stage 0 writes
 * an apostrophe as '@'. */
/* @0x10003480 */
void TV_CDECL Word_Classify(const char *word, Node *n)
{
    const char *w = word;
    int32_t len;

    n->flags &= 0xfffff0ffu;
    n->flags &= 0xffff0fffu;

    if (tv_strstr(g_words_article, w) != NULL) {
        n->flags = (n->flags & 0xfffff1ffu) | 0x100u;
        return;
    }
    if (tv_strstr(g_words_pronoun, w) != NULL) {
        n->flags = (n->flags & 0xfffff2ffu) | 0x200u;
        n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
        return;
    }
    if (tv_strstr(g_words_quant, w) != NULL) {
        n->flags = (n->flags & 0xfffff3ffu) | 0x300u;
        return;
    }
    if (tv_strstr(g_words_quant_pl, w) != NULL) {
        n->flags = (n->flags & 0xfffff3ffu) | 0x300u;
        n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
        return;
    }
    if (tv_strstr(g_words_conj, w) != NULL) {
        n->flags = (n->flags & 0xfffff4ffu) | 0x400u;
        if (tv_strcmp(w, g_word_that) == 0) {
            n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
            return;
        }
        if (tv_strcmp(w, g_word_which) == 0)
            n->flags = (n->flags & 0xffff2fffu) | 0x2000u;
        return;
    }
    if (tv_strstr(g_words_prep, w) != NULL) {
        n->flags = (n->flags & 0xfffff5ffu) | 0x500u;
        if (tv_strcmp(w, g_word_to) == 0)
            n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
        return;
    }
    if (tv_strstr(g_words_number, w) != NULL) {
        n->flags = (n->flags & 0xfffff6ffu) | 0x600u;
        if (tv_strcmp(w, g_word_one) == 0)
            n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
        return;
    }
    if (tv_strstr(g_words_poss, w) != NULL) {
        n->flags = (n->flags & 0xfffff8ffu) | 0x800u;
        return;
    }
    if (tv_strstr(g_words_be, w) != NULL) {
        n->flags = (n->flags & 0xfffffcffu) | 0xc00u;
        return;
    }
    if (tv_strstr(g_words_have, w) != NULL) {
        n->flags = (n->flags & 0xfffffdffu) | 0xd00u;
        return;
    }
    if (tv_strstr(g_words_demon, w) != NULL) {
        n->flags |= 0xf00u;
        if (tv_strcmp(w, g_word_this) != 0)
            n->flags = (n->flags & 0xffff1fffu) | 0x1000u;
        return;
    }

    for (len = 0; w[len] != '\0'; len++)
        ;
    /* a contracted "'ve" behaves like "have", a contracted "'s" like "is" */
    if (len >= 6 && w[len - 4] == '@' && w[len - 3] == 'V' && w[len - 2] == 'E')
        n->flags = (n->flags & 0xfffffdffu) | 0xd00u;
    if (len < 5)
        return;
    if (w[len - 3] != '@' || w[len - 2] != 'S')
        return;
    if (tv_strcmp(w, g_word_lets) != 0)
        n->flags = (n->flags & 0xfffffeffu) | 0xe00u;
}

#if defined(TV_HOOK_BUILD)
#include <windows.h>
/* The lexicon is shared between engine instances, so lookups are
 * serialised. */
/* @0x10148b38 */
extern CRITICAL_SECTION g_lexicon_cs;
void Lexicon_Lock(void) { EnterCriticalSection(&g_lexicon_cs); }
void Lexicon_Unlock(void) { LeaveCriticalSection(&g_lexicon_cs); }
#else
void Lexicon_Lock(void) {}
void Lexicon_Unlock(void) {}
#endif

/* bsearch comparator over LexEntry.word. */
/* @0x10003980 */
int32_t TV_CDECL UserLex_Compare(const void *a, const void *b)
{
    return tv_strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* @0x100c8aa0 with the signed index the original uses; see stage0.c */
uint8_t Phone_Attr(int32_t idx);

/* Look the word between st->d14 and st->d18 up in the user lexicon.  On a hit the
 * letter nodes are replaced by the phoneme nodes of the entry: digits in the
 * entry are stress marks for the phoneme before them, and a word with no
 * stress mark of its own gets one on its first stressable vowel. */
/* @0x100039c0 */
uint8_t TV_THISCALL UserLex_Try(Engine *self)
{
    StageCtx *st = &self->stage_ctx[1];
    char word[41];
    char pron[44];
    const LexEntry *hit;
    const char *key = word;
    Node *n, *cur, *stressed = NULL, *fallback = NULL;
    int32_t i, len, keep_end = 0, keep_start = 0;
    uint8_t stress_set = 0;
    uint8_t c, lvl;

    if (self->s1_next_end != st->d18)
        keep_end = 1;
    if (self->s1_letters != st->d14)
        keep_start = 1;

    n = st->d14;
    i = 0;
    while (st->d18->next != n) {
        if (i >= 40)
            return 0;
        word[i] = (char)n->value;
        i++;
        n = n->next;
    }
    word[i] = '\0';

    Lexicon_Lock();
    hit = (const LexEntry *)tv_bsearch(&key, g_lexicon, g_lexicon_count,
                                       sizeof(LexEntry), UserLex_Compare);
    if (hit != NULL) {
        const char *p = hit->pron;
        for (i = 0; (pron[i] = p[i]) != '\0'; i++)
            ;
    }
    Lexicon_Unlock();
    if (hit == NULL)
        return 0;

    /* Drop the letters and build the phonemes in their place. */
    n = st->d14;
    while (st->d18 != n)
        n = Engine_NodeFree(self, n, 1);
    cur = Engine_NodeFree(self, n, 0);
    st->d14 = cur;

    for (len = 0; pron[len] != '\0'; len++)
        ;
    for (i = 0; i < len; i++) {
        c = (uint8_t)pron[i];
        if (c == '1' || c == '2' || c == '0')
            continue;
        cur = Engine_NodeAlloc(self, cur, 1, 3, c);
        c = (uint8_t)pron[i + 1];
        if (c == '1') {
            stress_set = 1;
            if (self->s1_1c2a == 1)
                lvl = 3;
            else
                lvl = (uint8_t)(self->s1_1c2b == 1 ? 1 : 2);
            i++;
            cur->flags = (cur->flags & ~0x18u) |
                         ((uint32_t)((int32_t)(int8_t)lvl << 3) & 0x18u);
            continue;
        }
        if (c == '2') {
            cur->flags = (cur->flags & ~0x10u) | 8u;
            i++;
            continue;
        }
        if (c == '0') {
            i++;
            continue;
        }
        if (stress_set == 1)
            continue;
        if (!(Phone_Attr((int8_t)cur->value) & 1))
            continue;
        if (stressed != NULL)
            continue;
        if (Phone_Attr((int16_t)((int16_t)(int8_t)cur->value | 0x200)) & 0x40) {
            if (fallback == NULL)
                fallback = cur;
        } else {
            stressed = cur;
        }
    }

    n = st->d14;
    st->d18 = cur;
    st->d14 = n->next;
    if (keep_start == 0)
        self->s1_letters = n->next;
    if (keep_end == 0)
        self->s1_next_end = cur;

    if (stress_set == 1)
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

#if !defined(TV_HOOK_BUILD)
/* The user lexicon itself: see the note in engine.h. */
LexEntry g_lexicon[0x1388];
uint32_t g_lexicon_count;
#endif

/* Set whenever the user lexicon changes, so the SAPI layer knows to save it. */
/* @0x1012c4f4 */ extern int32_t g_lexicon_dirty;

/*
 * Add a word to the user lexicon.
 *
 * The spelling is upper-cased and both strings are copied onto the engine's
 * own heap, since the caller keeps its own.  A word that is already there
 * has its pronunciation replaced; a new one goes on the end and the table is
 * sorted again, because lookups binary-search it.
 */
/* @0x10003d00 */
void TV_CDECL UserLex_Add(const char *word, const char *pron)
{
    LexEntry *tab = g_lexicon;
    uint32_t *count = &g_lexicon_count;
    LexEntry *hit;
    char *w, *p;

    if (*count >= 0x1388u)
        return;

    w = (char *)tv_new(strlen(word) + 1);
    strcpy(w, word);
    tv_strupr(w);
    p = (char *)tv_new(strlen(pron) + 1);
    strcpy(p, pron);

    Lexicon_Lock();
    hit = (LexEntry *)tv_bsearch(&w, tab, *count, sizeof(LexEntry),
                                 UserLex_Compare);
    if (hit != NULL) {
        tv_delete((void *)hit->word);
        tv_delete((void *)hit->pron);
        hit->word = w;
        hit->pron = p;
    } else {
        tab[*count].word = w;
        tab[*count].pron = p;
        (*count)++;
        tv_qsort(tab, *count, sizeof(LexEntry), UserLex_Compare);
    }
    Lexicon_Unlock();
    g_lexicon_dirty = 1;
}
