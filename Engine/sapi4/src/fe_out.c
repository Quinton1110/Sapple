// The front end's phrase hand-over: the phone list of a phrase, with the index escapes turned back into
// the tags they stood for (as "\Chr=...\" style records the unit stage passes on), pronunciation tags
// ("\Tp") converted through the phone converter, queued for the back end.
#include "fe_input.h"
#include "fe_phones.h"
#include "fe_small.h"
#include "queue.h"
#include "crt_vc.h"
#include "vcrt.h"
#include "x87.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int32_t fe_set_pitch_level(FrontEnd *fe, uint16_t v) {
    int32_t p;
    double base = (double)(uint32_t)fe->pitch;
    if (v == 0xffff) {
        p = x87_ftol(base + base);
        fe->pitch_set = p;
    } else if (v == 0) {
        p = x87_ftol(base * (double)0.5f);
        fe->pitch_set = p;
    } else {
        if ((uint32_t)v > (uint32_t)x87_ftol(base + base)) return (int32_t)0x8000ffffu;
        if ((uint32_t)v < (uint32_t)x87_ftol(base * (double)0.5f)) return (int32_t)0x8000ffffu;
        fe->pitch_set = v;
        p = v;
    }
    words_set_pitch(fe->words, p);
    vc_PostMessageA(fe->hwnd, fe->msg, 1, 0);
    return 0;
}

// a tag record: prefix, the entry's text, a closing backslash
static void tag_rec(PhoneIn **out, int32_t *count, uint32_t prefix, const char *text) {
    char *d = (char *)(void *)*out;     // (a tag may run past the name into the record's other fields)
    strcpy(d, DLLVAR(const char, prefix));
    strcat(d, text);
    strcat(d, DLLVAR(const char, 0x6371f18c));     // "\"
    (*out)++;
    (*count)++;
}

// pitch in quarter semitones above the base, as Hz
static float semis(double v, int32_t base) {
    return (float)(pow(2.0, v * 0.020833333333333332) * (double)base);
}

// the phone converter's part of a "\Tp" tag
typedef struct TpFrame {
    uint16_t w[4];              // the phone codes read (%x %x %x)
    int32_t res;                // -1 when converted
    uint16_t name[0x14];        // the phone name, wide
} TpFrame;

int32_t fe_phrase_out(FrontEnd *fe, int32_t n) {
    int32_t rel_dur = 1, rel_pitch = 1, extra = 0, bp = 0, ep = 0, base = 0;
    if (n == 0) return 0;
    PhoneIn *a = vc_calloc((size_t)(uint32_t)n, 0x64);
    int32_t n2 = phones_build(fe->words, a, fe->single_char, fe->_8c);
    int32_t cap = n2 + fe->nindex * 2;
    PhoneIn *b = vc_calloc((size_t)(uint32_t)cap, 0x64);
    PhoneIn *out = b, *cur = a;
    int32_t last = fe->first_index - 1, k = 0, count = 0;
    for (int32_t i = 0;; i++) {
        if (i >= n2) {
            if (k >= fe->nindex) break;
        } else if (cur->name[0] != '\\') {
            if (fe->in_tp) extra--;
            else {
                memcpy(out, cur, 0x64);
                if (fe->flags & 0x100u)
                    for (int32_t c = 0; c < out->nstates; c++) out->b[c] = (float)(double)(uint32_t)fe->pitch_set;
                out++;
                count++;
            }
            cur++;
            continue;
        }
        k++;
        if (i < n2) {
            last = vc_atoi(cur->name + 1);
            cur++;
        } else last++;
        FeEntry *e = &GP(FeEntry, fe->entries)[last];
        switch (e->type) {
        case 0x13:
            base = vc_atoi(GP(char, e->text));
            continue;
        case 0x14:
            if (extra > 0) {
                cap += extra;
                b = vc_realloc(b, (size_t)(uint32_t)cap * 0x64);
                memset((uint8_t *)b + count * 0x64, 0, (size_t)(uint32_t)extra * 0x64);
                out = b + count;
            }
            fe->in_tp = 0;
            continue;
        case 0x15:
            break;
        default:
            if (fe->in_tp) {
                extra--;
                continue;
            }
            switch (e->type) {
            case 0: {
                int32_t v = tag_char_value(GP(const char, e->text));
                if (v == 0) {
                    fe->flags = 0;
                    if (fe->f50 < 0) fe_voice_flags(fe, -fe->f50);
                } else if (v == 0x100) fe->flags |= 0x100u;
                tag_rec(&out, &count, 0x63721b2c, GP(const char, GP(FeEntry, fe->entries)[last].text));   // "\Chr="
                break;
            }
            case 1: tag_rec(&out, &count, 0x63721b34, GP(const char, e->text)); break;      // "\Wrd="
            case 9: tag_rec(&out, &count, 0x63721b3c, GP(const char, e->text)); break;      // "\Mrk="
            case 0xa:
                strcpy(out->name, DLLVAR(const char, 0x6371c07c));      // "#"
                out++;
                strcpy(out->name, DLLVAR(const char, 0x63721b44));      // "\Pau="
                strcat(out->name, GP(const char, GP(FeEntry, fe->entries)[last].text));
                strcat(out->name, DLLVAR(const char, 0x6371f18c));
                out->dur = 0.0f;
                out++;
                count += 2;
                strcpy(out->name, DLLVAR(const char, 0x6371c07c));
                out++;
                count++;
                break;
            case 0xf:
                fe->flags = 0;
                if (fe->f50 < 0) fe_voice_flags(fe, -fe->f50);
                strcpy(out->name, DLLVAR(const char, 0x63721b4c));      // "\RST\"
                out++;
                count++;
                break;
            case 0x10: tag_rec(&out, &count, 0x63721b54, GP(const char, e->text)); break;   // "\Spd="
            case 0x12: tag_rec(&out, &count, 0x63721b5c, GP(const char, e->text)); break;   // "\Vol="
            default: break;
            }
            continue;
        }
        // a "\Tp" tag: "=n <RD,RP,BP=b,EP=e> code code code, duration (a,Pp,Vv)(...); ..."
        {
            int32_t fail = 0, tpv;
            char *t = str_dup0(GP(const char, e->text));
            for (char *u = t; *u; u++) *u = (char)vc_toupper((uint8_t)*u);     // _strupr
            int r = vc_scan_eq_int(t, &tpv);
            char *q = strchr(t, '<');
            if (r == 0 || !q) continue;     // (the original leaves t allocated here)
            do {
                q++;
                size_t l = strcspn(q, DLLVAR(const char, 0x63721b28));     // ",>"
                if (l == 2 && q[0] == 'R') {
                    if (q[1] == 'D') rel_dur = 0;
                    else if (q[1] == 'P') rel_pitch = 0;
                } else if (l > 3) {
                    if (strncmp(q, DLLVAR(const char, 0x63721b24), 3) == 0) bp = vc_atoi(q + 3);        // "BP="
                    else if (strncmp(q, DLLVAR(const char, 0x63721b20), 3) == 0) ep = vc_atoi(q + 3);   // "EP="
                }
                q += l;
            } while (*q != '>');
            void *conv = NULL;
            TpFrame *f = vc_stack_push(sizeof(TpFrame), 0);
            int32_t saved_extra = extra, saved_count = count;
            PhoneIn *saved_out = out;
            if (vc_CoCreateInstance(0x6369e738, 7, 0x6369e768, &conv) < 0) goto tp_fail;
            {
                uint32_t hdr[1] = { GHOST(DLLVAR(const char, 0x63721598)) };      // L"[Header] ..."
                if (vc_com_call(conv, 6, 1, hdr) < 0) goto tp_fail;
            }
            q += 1 + strspn(q + 1, DLLVAR(const char, 0x63721b18));
            {
                int32_t off = count * 0x64, cap5 = count + 5, off5 = (count + 5) * 0x64;
                uint32_t x0 = 0, x1 = 0, x2 = 0;
                for (;;) {
                    int nx = sscanf(q, DLLVAR(const char, 0x63721b0c), &x0, &x1, &x2);     // "%x %x %x"
                    f->w[0] = (uint16_t)x0;
                    if (nx >= 0 && nx < 4) f->w[nx] = 0;
                    if (nx == 3) f->w[2] = (uint16_t)x2;
                    if (nx >= 2) f->w[1] = (uint16_t)x1;
                    else if (nx == 0) vc_DebugBreak();
                    f->res = (int32_t)x0;
                    uint32_t args[5] = { GHOST(f->w), GHOST(&f->res), GHOST(f->name), 0x13, 0 };
                    vc_com_call(conv, 3, 5, args);
                    if (f->res != -1) goto tp_fail;
                    {   // WideCharToMultiByte into the 15 bytes of the name, or nothing if it does not fit
                        size_t wl = 0;
                        while (f->name[wl]) wl++;
                        if (wl + 1 <= 0xf)
                            for (size_t j = 0; j <= wl; j++) out->name[j] = (char)uni_to_cp1252(f->name[j]);
                    }
                    if (out->name[0] == '#') {
                        int32_t v = base >= 0 ? (int32_t)(q - t) + base : (int32_t)((q - t) << 1) - base;
                        sprintf(out->name, DLLVAR(const char, 0x63721b00), v);     // "\Wrd=%i\"
                        off += 0x64;
                        out++;
                        count++;
                        cap5++;
                        off5 += 0x64;
                        goto grow;
                    }
                    if (out->name[0] == '-') goto skip;
                    q += strcspn(q, DLLVAR(const char, 0x63721afc));     // ",;"
                    if (*q == ';') goto adv;
                    q += 1 + strspn(q + 1, DLLVAR(const char, 0x63721af4));
                    if (*q != '(') {
                        int32_t d = vc_atoi(q);
                        out->dur = rel_dur ? (float)-(double)d : (float)((double)5.0f - (double)d * (double)-0.01f);
                        q += strcspn(q, DLLVAR(const char, 0x63721aec));
                        q += strspn(q, DLLVAR(const char, 0x63721ae4));
                    }
                    out->nstates = 0;
                    if (bp) {
                        out->a[0] = 0.0f;
                        out->b[0] = rel_pitch ? (float)bp : semis((double)bp, fe->pitch_set);
                        bp = 0;
                        out->nstates++;
                    }
                    while (*q == '(') {
                        if (out->nstates >= 5) out->nstates = 4;
                        int32_t ns = out->nstates;
                        out->a[ns] = (float)(atof(q + 1) * (double)0.01f);
                        if (x87_ja(out->a[ns], 1.0)) out->a[ns] = 1.0f;
                        else if (!x87_jae(out->a[ns], 0.0)) out->a[ns] = 0.0f;
                        if (ns != 0 && x87_jb(out->a[ns], out->a[ns - 1])) {
                            fail = 1;
                            goto adv;
                        }
                        q = strchr(q, ',');
                        do {
                            q++;
                            q += strspn(q, DLLVAR(const char, 0x63721ae4));
                            char c = *q++;
                            double v = atof(q);
                            if (c == 'P') out->b[ns] = rel_pitch ? (float)v : semis(v, fe->pitch_set);
                            else if (c == 'V') out->c[ns] = (float)(v * (double)0.01f);
                            q += strcspn(q, DLLVAR(const char, 0x63721ae0));     // ",)"
                        } while (*q == ',');
                        out->nstates++;
                        q += strcspn(q, DLLVAR(const char, 0x63721adc));     // "(;"
                    }
                adv:
                    off += 0x64;
                    off5 += 0x64;
                    out++;
                    count++;
                    cap5++;
                    extra++;
                grow:
                    if (count >= cap) {
                        cap = cap5;
                        b = vc_realloc(b, (size_t)(uint32_t)off5);
                        memset((uint8_t *)b + off, 0, 0x1f4);
                        out = (PhoneIn *)(void *)((uint8_t *)b + off);
                    }
                skip:
                    q = strchr(q, ';');
                    if (q) q += 1 + strspn(q + 1, DLLVAR(const char, 0x63721af4));
                    if (fail) goto tp_restore;
                    if (!q || !*q) goto tp_done;
                }
            }
        tp_fail:
            fail = 1;
        tp_restore:
            count = saved_count;
            out = saved_out;
            extra = saved_extra;
        tp_done:
            vc_stack_pop(f, sizeof(TpFrame), 0);
            if (conv) vc_com_release(conv);     // (with no converter the original releases a null object: a fault)
            vc_free(t);
            if (fail) continue;
            fe->in_tp = 1;
            if (ep == 0) continue;
            PhoneIn *pr = out - 1;
            while (pr->name[0] == '\\') pr--;
            if (!x87_je(1.0, pr->a[pr->nstates])) pr->nstates++;
            if (pr->nstates >= 5) pr->nstates = 4;
            pr->a[pr->nstates] = 1.0f;
            pr->b[out->nstates] = rel_pitch ? (float)ep : semis((double)ep, fe->pitch_set);
        }
    }
    vc_free(a);
    QItem *it = vc_malloc(sizeof(QItem));
    if (!it) return 0;
    memset(it, 0, sizeof(QItem));
    it->code = 3;
    it->bytes = (uint32_t)(count * 0x64);
    GPSET(it->data, (void *)b);
    if (vc_WaitForSingleObject(fe->abort_event, 0) == 0) {
        vc_free(GP(void, it->data));
        vc_free(it);
    } else {
        queue_push(GP(Queue, fe->queue), it);
        vc_free(it);
    }
    return 0;
}

// one text (a code 1 item just taken from the text queue): prepared, then read a part at a time and
// every phrase's phone list queued; "\x1bI" escapes in the text change the pitch or end the text
void fe_text_item(FrontEnd *fe, FeThreadFrame *f) {
    fe->single_char = 0;
    if (locked_get_58(fe)) {
        if (f->item.data) vc_free(GP(void, f->item.data));
        return;
    }
    fe->in_tp = 0;
    if (!f->item.data) return;
    *DLLVAR(int32_t, 0x63738b80) = 1;
    if (fe_text_input(fe, GP(const uint16_t, f->item.data), &f->res, (int32_t)f->item.a.value, (int32_t)f->item.v2) < 0)
        return;
    if (f->item.data) vc_free(GP(void, f->item.data));
    f->item.data = 0;
    for (;;) {
        if (GP(char, fe->next) > GP(char, fe->text_last) + 1) {
            vc_free(GP(char, fe->text));
            return;
        }
        if (f->res != 0x6f) {
            for (;;) {
                int32_t n = words_phrase(fe->words);
                if (vc_WaitForMultipleObjects(2, f->h2, 0, 0xffffffffu) == 0) {
                    words_abort(fe->words);
                    break;
                }
                fe_phrase_out(fe, n);
                f->res = fe_continue(fe);
                if (locked_get_58(fe)) break;
                if (f->res == 0x6f) break;
            }
        }
        // the next part: after an index escape "\x1bI<n> " (0: the end of the text, else a pitch)
        char *nx = GP(char, fe->next);
        if (nx >= GP(char, fe->text_last)) {
            GPSET(fe->next, nx + 1);
            continue;
        }
        nx += 2;
        GPSET(fe->next, nx);
        if (*nx != ' ') {
            char *d = f->arg;
            do {
                *d = *nx;
                d[1] = 0;
                d++;
                nx++;
            } while (*nx != ' ');
        }
        GPSET(fe->next, nx + 1);
        if (vc_lstrcmpA(f->arg, DLLVAR(const char, 0x6371da14)) == 0) fe_text_end(fe);     // "0"
        else fe_set_pitch_level(fe, (uint16_t)vc_atoi(f->arg));
        fe->cur = fe->next;
        char *p = GP(char, fe->next);
        while (p <= GP(char, fe->text_last)) {
            if (*p == 0x1b && p[1] == 'I') break;
            p++;
            GPSET(fe->next, p);
        }
        *GP(char, fe->next) = 0;
        int32_t r = words_feed(fe->words, GP(char, fe->cur), &fe->f28, &fe->f2c);
        f->res = fe_after_feed(fe, r);
    }
}

// @0x63684233 thiscall: the front end's thread: texts from the text queue, phone lists (and the other
// items, passed on) to the output queue; the abort event drops everything
int32_t fe_thread(FrontEnd *fe) {
    FeThreadFrame f;
    vc_CoInitialize();
    f.h[0] = fe->abort_event;
    f.h2[0] = fe->abort_event;
    f.h[1] = GP(Queue, fe->queue_in)->ev_nonempty;
    f.h2[1] = GP(Queue, fe->queue)->ev_room;
    for (;;) {
        if (vc_WaitForMultipleObjects(2, f.h, 0, 0xffffffffu) == 0) break;
        if (!queue_pop(GP(Queue, fe->queue_in), &f.item)) continue;
        if (f.item.code != 1) {
            if (vc_WaitForMultipleObjects(2, f.h2, 0, 0xffffffffu) == 0) {
                if (f.item.data) vc_free(GP(void, f.item.data));
            } else queue_push(GP(Queue, fe->queue), &f.item);
            continue;
        }
        fe_text_item(fe, &f);
    }
    // aborted: drop the texts still queued
    while (queue_pop(GP(Queue, fe->queue_in), &f.item)) {
        if (f.item.code == 9 && f.item.a.obj) vc_com_release(GP(void, f.item.a.obj));
        if (f.item.data) vc_free(GP(void, f.item.data));
    }
    vc_SetEvent(fe->done_event);
    vc_CoUninitialize();
    return 0;
}

int32_t fe_get_pitch(const FrontEnd *fe, uint16_t *v) {
    *v = (uint16_t)fe->pitch_set;
    return 0;
}
