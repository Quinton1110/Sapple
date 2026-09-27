/*
 * Feeding SAPI TextData items into the engine's input ring.
 */
#include "engine.h"
#include "crt.h"

static int ends_sentence(char c)
{
    return c == '.' || c == '!' || c == '?' || c == ',' || c == ';';
}

static int is_symbol(uint8_t c)
{
    switch (c) {
    case 0xa2: case 0xa3: case 0xa5: case 0xa7: case 0xa9: case 0xae: case 0xb1:
    case 0xb6: case 0xbc: case 0xbd: case 0xbe: case 0xd7: case 0xf7:
        return 1;
    default:
        return 0;
    }
}

static void seg_start(InputSeg *seg, const char *p)
{
    seg->len = 0;
    seg->text = (char *)p;
    seg->no_words = 1;
    seg->caps_only = 1;
    seg->flag_c = 0;
}

/* Copy (part of) a TextData item into the input ring; *ppos is the
 * position reached, kept across calls while the ring is full.
 *
 * With PreFormat on, the text is taken up to ten lines at a time and each
 * line is tidied: trailing blanks are dropped, lines without final
 * punctuation that are shorter than 40 characters get a period, lines with
 * no words in a text longer than 130 characters become just a period, and in
 * texts longer than 40 characters ALL-CAPS lines are lower-cased so they are
 * read as words rather than spelled.  ESC [ ... <letter> sequences pass
 * through untouched. */
/* @0x10055aa0 */
void TV_THISCALL Engine_Feed(Engine *self, const char *text, uint32_t len, uint32_t *ppos)
{
    uint32_t pos = *ppos;
    int16_t nseg, k, j, slen;
    InputSeg *seg;
    uint8_t c;

    if (!self->preformat) {
        for (; pos < len; pos++)
            if (!Engine_InPut(self, (uint8_t)text[pos]))
                break;
        if (pos >= len) {
            Engine_InPutEnd(self);
            self->item_done = 1;
        }
        goto done;
    }

    if (pos == 0)
        self->feed_pos = 0;
    nseg = 0;
    seg = &self->in_seg[0];
    seg_start(seg, text + self->feed_pos);
    while (self->feed_pos < len) {
        uint32_t p = self->feed_pos;
        c = (uint8_t)text[p];
        if (c == '\n' || c == '\r') {
            p++;
            self->feed_pos = p;
            if (text[p] == (c == '\n' ? '\r' : '\n'))
                self->feed_pos = ++p;
            while (seg->len > 0 &&
                   (seg->text[seg->len - 1] == ' ' || seg->text[seg->len - 1] == '\t'))
                seg->len--;
            nseg++;
            if (nseg >= 10)
                break;
            seg = &self->in_seg[nseg];
            seg_start(seg, text + self->feed_pos);
            continue;
        }
        if (c == 0x1b && text[p + 1] == '[') {
            self->feed_pos = p + 2;
            seg->len += 2;
            seg->no_words = 0;
            if (text[self->feed_pos] == '1' && text[self->feed_pos + 1] == 'I')
                seg->caps_only = 0;
            while (self->feed_pos < len) {
                int alpha = tv_isalpha((uint8_t)text[self->feed_pos]);
                self->feed_pos++;
                seg->len++;
                if (alpha)
                    break;
            }
            continue;
        }
        seg->len++;
        if (tv_islower(c) || c > 0x7f)
            seg->caps_only = 0;
        if (tv_isalnum(c) || c > 0xbf || is_symbol(c))
            seg->no_words = 0;
        self->feed_pos++;
    }
    if (nseg != 10) {
        while (seg->len > 0 &&
               (seg->text[seg->len - 1] == ' ' || seg->text[seg->len - 1] == 0 ||
                seg->text[seg->len - 1] == '\t'))
            seg->len--;
        if (seg->len > 0)
            nseg++;
    }

    for (k = 0; k < nseg; k++) {
        seg = &self->in_seg[k];
        if (seg->no_words && seg->len != 0 && len > 0x82) {
            Engine_InPut(self, '.');
            Engine_InPut(self, '\n');
            continue;
        }
        slen = seg->len;
        if (slen > 0) {
            for (j = 0; j < slen; j++) {
                if (seg->caps_only && len > 0x28) {
                    c = (uint8_t)seg->text[j];
                    if (c == 0x1b && seg->text[j + 1] == '[') {
                        /* copy the escape sequence up to its final letter */
                        while (slen > j) {
                            if (tv_isalpha((uint8_t)seg->text[j])) {
                                Engine_InPut(self, (uint8_t)seg->text[j]);
                                break;
                            }
                            j++;
                            Engine_InPut(self, (uint8_t)seg->text[j - 1]);
                        }
                        continue;
                    }
                    Engine_InPut(self, (uint8_t)tv_tolower(c));
                } else {
                    Engine_InPut(self, (uint8_t)seg->text[j]);
                }
            }
            if (slen < 0x28 && !ends_sentence(seg->text[slen - 1]))
                Engine_InPut(self, '.');
        }
        Engine_InPut(self, '\n');
    }
    pos = self->feed_pos;
    if (pos >= len) {
        Engine_InPutEnd(self);
        self->item_done = 1;
    }
done:
    if (*ppos < pos)
        self->st_input_empty = 0;
    *ppos = pos;
}

/* Move pending input on towards the pipeline: through TextIn when it is
 * enabled, otherwise straight into the preformatter (at most 400
 * characters per call).  new_item is set for the first flush of a new
 * TextData item. */
/* @0x10055f50 */
void TV_THISCALL Engine_Flush(Engine *self, int32_t new_item)
{
    TextIn *ti = self->textin;
    int32_t n, c;

    if (self->textin_on && ti != NULL) {
        ti->input_done = self->st_input_empty;
        ti->item_done = self->item_done;
        if ((uint8_t)new_item)
            TextIn_Reset(ti);
        TextIn_Flush(ti, 0);
        return;
    }
    for (n = 0;;) {
        c = Engine_InGet(self);
        if (c < 0) {
            self->st_input_empty = 1;
            break;
        }
        n++;
        Preformat_PutChar(self, (uint8_t)c);
        if (n >= 400)
            break;
    }
    if (n > 0)
        self->st_idle = 0;
}
