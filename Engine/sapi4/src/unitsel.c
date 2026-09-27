#include "unitsel.h"
#include "tags.h"
#include "crt_vc.h"
#include "x87.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

LAYOUT(UnitRec, dur, 0x08);
LAYOUT(UnitRec, c, 0x38);
_Static_assert(sizeof(UnitRec) == 0x4c, "UnitRec");
LAYOUT(UnitStage, sil_phone, 0x2a0);
LAYOUT(UnitStage, tree_mode, 0x298);
LAYOUT(TreeQuery, _2c, 0x2c);

static int is_mark(char ch) { return ch == '\\' || ch == '#'; }

void unit_select(UnitStage *u, const char *ph, int n) {
    int count = 0, prev = u->sil_phone, end = 0, begin = 0;
    for (int i = 0; i < n; i++)
        if (!is_mark(ph[100 * i])) count++;
    if (u->cap < count) {
        u->cap = count;
        GPSET(u->recs, (UnitRec *)vc_realloc(GP(void, u->recs), (size_t)(uint32_t)(count * 0x4c)));
        GPSET(u->ids, (int16_t *)vc_realloc(GP(void, u->ids), (size_t)(uint32_t)(u->cap * 2)));
        GPSET(u->senones, (int16_t *)vc_realloc(GP(void, u->senones), (size_t)(uint32_t)(u->cap * 2)));
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const char *p = ph + 100 * i;
        if (is_mark(*p)) continue;
        int16_t id = (int16_t)named_index(GP(const NamedTable, u->phone_names), p);
        GP(int16_t, u->ids)[k] = id;
        GP(UnitRec, u->recs)[k].phone = GP(int16_t, u->ids)[k];
        k++;
    }
    k = 0;
    for (int i = 0; i < n; i++) {
        const char *p = ph + 100 * i;
        if (is_mark(*p)) {
            begin = 1;
            continue;
        }
        if (is_mark(p[100])) end = 1;   // (also reads the slot after the last phone, as the original)
        int right = k + 1 < count ? GP(int16_t, u->ids)[k + 1] : u->sil_phone;
        int8_t pos = end ? (begin ? 's' : 'e') : (begin ? 'b' : 0);
        UnitRec *r = GP(UnitRec, u->recs) + k;
        int32_t s;
        if (u->tree_mode == 1) {
            s = senone_lookup(GP(const SenoneTree, u->tree), r->phone, prev, right, pos, 0);
        } else {
            TreeQuery q;
            memset(&q, 0, sizeof q);   // (the original leaves the unnamed bytes as stack garbage)
            q.cur = (int16_t)r->phone;
            q.left = (int16_t)prev;
            q.right = (int16_t)right;
            q.pos = pos;
            q._2c = 0;
            s = tree_query(GP(const SenoneTree, u->tree), &q);
        }
        r->senone = s;
        end = begin = 0;
        GP(int16_t, u->senones)[k] = (int16_t)r->senone;
        prev = GP(int16_t, u->ids)[k];
        k++;
    }
}

LAYOUT(UnitStage, recs, 0x25c);
LAYOUT(UnitStage, amp, 0x28c);
LAYOUT(UnitOut, c, 0x3c);
_Static_assert(sizeof(UnitOut) == 0x50, "UnitOut");
_Static_assert(sizeof(PhoneIn) == 0x64, "PhoneIn");
LAYOUT(SenoneTree, phone_units, 0x30);
LAYOUT(SenoneTree, nstates, 0x38);

int unit_prosody(UnitStage *u, const PhoneIn *ph, int n, int32_t *last, int next_phone, UnitOutBuf *out) {
    const double K = 0.0;
    float scale = (float)((double)u->amp * (double)1.5259021893143654e-05f);
    UnitRec *recs = GP(UnitRec, u->recs);
    for (int i = 0; i < n; i++) {
        UnitRec *r = recs + i;
        r->dur = ph[i].dur;
        r->nstates = ph[i].nstates;
        for (int k = 0; k < r->nstates; k++) {
            r->a[k] = ph[i].a[k];
            r->b[k] = ph[i].b[k];
            r->c[k] = ph[i].c[k];
        }
    }
    for (int i = 0; i < n; i++) {
        UnitRec *r = recs + i;
        if (r->phone == u->sil_phone) {
            for (int k = 0; k < r->nstates; k++) r->c[k] = 1.0f;
            if (x87_je(K, r->dur)) r->dur = 1.0f;
            if (!x87_jbe(K, r->dur)) {
                r->dur = (float)(-(double)u->dur_scale * r->dur * 0.001);
            } else if (!x87_jbe(r->dur, 5.0)) {
                r->dur = (float)(((double)r->dur - 5.0) * u->dur_scale + u->sil_dur);
            } else if (!x87_jae(K, r->dur)) {
                r->dur = (float)((double)u->dur_scale * r->dur + u->sil_dur);
            }
        } else {
            const SenStat *st = GP(const SenStat, u->stats[r->phone]) + r->senone;
            for (int k = 0; k < r->nstates; k++) {
                if (x87_je(K, r->c[k])) r->c[k] = (float)sqrt((double)st->var);
                else if (!x87_jae(K, r->c[k])) r->c[k] = (float)(sqrt((double)st->var) * r->c[k]);
            }
            if (x87_je(K, r->dur)) r->dur = 1.0f;
            if (!x87_jbe(K, r->dur)) {
                r->dur = (float)((double)r->dur * (double)-0.001f);
            } else if (!x87_jbe(r->dur, 5.0)) {
                r->dur = (float)(((double)r->dur - 5.0) * st->scale * (double)0.001f);
            } else {
                r->dur = (float)((double)u->dur_scale * st->scale * r->dur);
                r->dur = (float)((double)r->dur * (double)0.001f);
            }
        }
    }
    unit_prosody_a(u, n);
    unit_prosody_b(u, n);
    out->bytes = n;
    if (u->final_sil && next_phone != u->sil_phone) out->bytes = n + 1;
    out->bytes *= 0x50;
    UnitOut *o = vc_malloc((size_t)(uint32_t)out->bytes);
    GPSET(out->units, o);
    recs = GP(UnitRec, u->recs);
    const SenoneTree *tree = GP(const SenoneTree, u->tree);
    for (int i = 0; i < n; i++, o++) {
        const UnitRec *r = recs + i;
        strcpy(o->name, GP(const char, named_item(GP(const NamedTable, u->phone_names), r->phone)));
        if (r->phone == u->sil_phone) {
            o->unit = 0;
        } else {
            int16_t s = GP(int16_t, u->senones)[i];
            if (s > tree->max_senone) o->unit = s;
            else o->unit = (int16_t)(GP(const uint16_t, tree->phone_units)[r->phone * 2] - (uint16_t)u->senone_base +
                                     (uint16_t)r->senone + 1);
        }
        o->nstates = r->nstates;
        o->dur = r->dur;
        for (int k = 0; k < o->nstates; k++) {
            o->a[k] = r->a[k];
            o->b[k] = (int16_t)x87_ftol((double)r->b[k]);
            o->c[k] = (float)((double)r->c[k] * scale);
        }
    }
    if (u->final_sil && next_phone != u->sil_phone) {
        strcpy(o->name, "SIL");
        o->unit = 0;
        o->nstates = 0;
        memcpy(&o->dur, &u->sil_dur, 4);
        o->a[0] = 1.0f;
        o->b[0] = o[-1].b[o[-1].nstates - 1];
        o->c[0] = 0.0f;
    }
    *last = recs[n - 1].phone;
    return 0;
}

LAYOUT(UnitStage, last_c, 0x2b8);

// @0x63686c7d: fill in the unspecified (0) b[] targets of every state by interpolating in time between
// the specified ones, inserting a state at a unit's start (a == 0 there) and end (a == 1) where needed.
// Written as the original's control flow (labels = its basic blocks) because the running time lives on
// the x87 stack across them and is sometimes re-read from a float copy: the roundings are the point.
void unit_prosody_a(UnitStage *u, int n) {
    UnitRec *R;
    const double Z = 0.0;
    float a28 = 0.0f, T10 = 0.0f, Tn = 0.0f, Tc, bval, last;
    int i = 0, s = 0, cur = 0, carry = 0, last_i = n - 1;
    int found, i_end, s_end, j, lim, cnt, found2, i2;
    double t = 0.0;
top:
    R = GP(UnitRec, u->recs);
    if (i < last_i) goto a;
    if (i != last_i) goto end;
    if (s < R[n - 1].nstates) goto a;
    if (R[n - 1].nstates > 1) goto end;
a:
    found = 0;
    T10 = (float)t;
    if (i >= n) goto b;
    for (;;) {
        if (s < R[i].nstates) {
            for (;;) {
                if (!x87_je(R[i].b[s], Z)) { found = 1; break; }
                s++;
                if (s >= R[i].nstates) break;
            }
        }
        if (found) goto f;
        if (i + 1 != n) s = 0;
        t = t + R[i].dur;
        i++;
        if (i >= n) break;
    }
b:
    if (!found) goto g;
f: {
        double v;
        if (!x87_je(Z, R[i].a[s])) {
            v = (double)R[i].dur * R[i].a[s];
            Tn = (float)(v + t);
        } else if (i == 0) {
            Tn = (float)t;
        } else {
            if (R[i - 1].nstates == 0) v = R[i - 1].dur;
            else v = (double)R[i - 1].a[R[i - 1].nstates - 1] * R[i - 1].dur;
            Tn = (float)(v + t);
        }
        bval = R[i].b[s];
        if (u->last_b == -1) u->last_b = x87_ftol(R[i].b[s]);
        goto m;
    }
g:
    Tn = (float)t;
    if (u->last_b == -1) u->last_b = 0x6e;
    i = last_i;
    bval = (float)(double)u->last_b;
    s = R[last_i].nstates;
m:
    s_end = s;
    t = T10;
    Tc = T10;
    i_end = i;
    last = -1.0f;
    j = cur;
    if (cur > i_end) goto nn;
    for (;;) {
        R = GP(UnitRec, u->recs);
        UnitRec *r = R + j;
        int nst = r->nstates;
        if (nst == 0) {
            r->nstates = nst + 1;
            r->a[0] = 0.0f; r->b[0] = 0.0f; r->c[0] = 0.0f;
            if (j == i_end) s_end++;
        } else if (!x87_je(Z, r->a[0])) {
            if (nst == 5) r->nstates = 4;
            memmove(&r->a[1], &r->a[0], (size_t)(uint32_t)(r->nstates * 4));
            memmove(&r->b[1], &r->b[0], (size_t)(uint32_t)(r->nstates * 4));
            memmove(&r->c[1], &r->c[0], (size_t)(uint32_t)(r->nstates * 4));
            r->a[0] = 0.0f; r->b[0] = 0.0f; r->c[0] = 0.0f;
            r->nstates++;
            if (j == i_end) s_end++;
            t = Tc;
        }
        nst = r->nstates;
        if (!x87_je(r->a[nst - 1], 1.0)) {
            if (nst == 5) r->nstates = 4;
            r->a[r->nstates] = 1.0f;
            r->b[r->nstates] = 0.0f;
            r->c[r->nstates] = 0.0f;
            r->nstates++;
        }
        lim = j == i_end ? s_end : r->nstates;
        int ecx = carry;
        if (carry < lim) {
            cnt = lim - carry;
            for (int k = carry; cnt; k++, cnt--) {
                if (!x87_je(Z, r->b[k])) {
#ifdef DECOMP_HOOK
                    decomp_warn("unit_prosody_a: a specified b[] inside an interpolated stretch (the original calls DebugBreak)");
#endif
                    t = Tc;
                    continue;
                }
                double P0 = (double)u->last_b;
                double q = ((double)r->dur * r->a[k] + t - a28) * ((double)bval - P0);
                r->b[k] = (float)(q / ((double)Tn - a28) + P0);
                last = r->b[k];
            }
            ecx = lim;
        }
        carry = 0;
        if (ecx == r->nstates) {
            double x = (double)r->dur + t;
            Tc = (float)x;
            t = x;
        }
        j++;
        if (j > i_end) break;
    }
nn:
    if (!x87_je(-1.0, last)) u->last_b = x87_ftol(last);
    i2 = i_end;
    s = s_end;
    found2 = 1;
    R = GP(UnitRec, u->recs);
    while (i2 < n) {
        UnitRec *r = R + i2;
        int nst = r->nstates;
        if (nst == 0 || !x87_je(Z, r->a[0])) break;
        while (s < nst) {
            if (x87_je(Z, r->b[s])) { found2 = 0; break; }
            u->last_b = x87_ftol(r->b[s]);
            a28 = (float)((double)r->dur * r->a[s] + t);
            s++;
        }
        if (!found2) break;
        if (!x87_je(1.0, r->a[s - 1])) break;
        i2++;
        if (i2 != n) s = 0;
        t = t + r->dur;
    }
    carry = s;
    cur = i2;
    i = i2;
    goto top;
end:
    R = GP(UnitRec, u->recs);
    if (u->sil_phone == R[n - 1].phone) u->last_b = -1;
}

// @0x63686905: the same interpolation for the c[] targets (no states are inserted here); the value
// carried across phrases is a float (110 when there is none).
void unit_prosody_b(UnitStage *u, int n) {
    UnitRec *R;
    const double Z = 0.0;
    float a18 = 0.0f, T30 = 0.0f, T4, Tc, bval, last;
    int i = 0, s = 0, cur = 0, carry = 0, last_i = n - 1;
    int found, i_end, s_end, j, lim, found2, i2;
    double t = 0.0;
top:
    R = GP(UnitRec, u->recs);
    if (i < last_i) goto a;
    if (i != last_i) goto end;
    if (s < R[n - 1].nstates) goto a;
    if (R[n - 1].nstates > 1) goto end;
a:
    found = 0;
    T30 = (float)t;
    if (i >= n) goto b;
    for (;;) {
        if (s < R[i].nstates) {
            for (;;) {
                if (!x87_je(R[i].c[s], Z)) { found = 1; break; }
                s++;
                if (s >= R[i].nstates) break;
            }
        }
        if (found) goto f;
        if (i + 1 != n) s = 0;
        t = t + R[i].dur;
        i++;
        if (i >= n) break;
    }
b:
    if (!found) goto g;
f: {
        double v;
        if (!x87_je(Z, R[i].a[s])) {
            v = (double)R[i].dur * R[i].a[s];
            T4 = (float)(v + t);
        } else if (i == 0) {
            T4 = (float)t;
        } else {
            if (R[i - 1].nstates == 0) v = R[i - 1].dur;
            else v = (double)R[i - 1].a[R[i - 1].nstates - 1] * R[i - 1].dur;
            T4 = (float)(v + t);
        }
        bval = R[i].c[s];
        if (x87_je(-1.0, u->last_c)) u->last_c = R[i].c[s];
        goto m;
    }
g:
    T4 = (float)t;
    if (x87_je(u->last_c, -1.0)) u->last_c = 110.0f;
    bval = u->last_c;
    i = last_i;
    s = R[last_i].nstates;
m:
    t = T30;
    i_end = i;
    Tc = T30;
    s_end = s;
    last = -1.0f;
    j = cur;
    if (cur > i_end) goto nn;
    for (;;) {
        R = GP(UnitRec, u->recs);
        UnitRec *r = R + j;
        lim = j == i_end ? s_end : r->nstates;
        int eax = carry;
        if (carry < lim) {
            for (int k = carry, cnt = lim - carry; cnt; k++, cnt--) {
                if (!x87_je(Z, r->c[k])) {
#ifdef DECOMP_HOOK
                    decomp_warn("unit_prosody_b: a specified c[] inside an interpolated stretch (the original calls DebugBreak)");
#endif
                    t = Tc;
                    continue;
                }
                double P0 = (double)u->last_c;
                double q = ((double)r->dur * r->a[k] + t - a18) * ((double)bval - P0);
                r->c[k] = (float)(q / ((double)T4 - a18) + P0);
                last = r->c[k];
            }
            eax = lim;
        }
        carry = 0;
        if (eax == r->nstates) {
            double x = (double)r->dur + t;
            Tc = (float)x;
            t = x;
        }
        j++;
        if (j > i_end) break;
    }
nn:
    if (!x87_je(-1.0, last)) u->last_c = last;
    s = s_end;
    found2 = 1;
    i2 = i_end;
    R = GP(UnitRec, u->recs);
    while (i2 < n) {
        UnitRec *r = R + i2;
        int nst = r->nstates;
        if (nst == 0 || !x87_je(Z, r->a[0])) break;
        while (s < nst) {
            if (x87_je(Z, r->c[s])) { found2 = 0; break; }
            u->last_c = r->c[s];
            s++;
            a18 = (float)((double)r->dur * r->a[s - 1] + t);
        }
        if (!found2) break;
        if (!x87_je(1.0, r->a[s - 1])) break;
        i2++;
        if (i2 != n) s = 0;
        t = t + r->dur;
    }
    cur = i2;
    carry = s;
    i = i2;
    goto top;
end:
    R = GP(UnitRec, u->recs);
    if (u->sil_phone == R[n - 1].phone) u->last_c = -1.0f;
}

LAYOUT(UnitStage, notify_msg, 0x26c);
LAYOUT(UnitStage, fast_rate, 0x27c);
LAYOUT(UnitStage, alt_rate, 0x2c4);

static void fitted_scale(UnitStage *u, uint32_t base) {
    double x = (double)(uint32_t)(u->fast_rate - base) / ((double)u->fast_rate - 20.0) - -1.5;
    u->dur_scale = (float)x;
    if (!x87_jbe(x, 2.5)) u->dur_scale = 2.5f;
}
static double sil_from(const UnitStage *u) {
    return (1.0 / (double)u->rate - (double)u->dur_scale / (double)u->default_rate) * 55.0;
}

void unit_rate_update(UnitStage *u) {
    u->final_sil = u->fast_rate >= u->rate;
    if (u->rate_mode == 0) {
        if (!u->final_sil) {
            u->dur_scale = (float)((double)u->default_rate / (double)(int32_t)u->rate);
            u->sil_dur = 0.0f;
        } else {
            fitted_scale(u, u->rate);
            u->sil_dur = (float)sil_from(u);
        }
    } else if (!(u->rate_flags & 2)) {
        u->final_sil = 0;
        u->dur_scale = 1.0f;
        u->sil_dur = 0.0f;
    } else if (!u->final_sil) {
        u->dur_scale = (float)((double)u->alt_rate / (double)(int32_t)u->rate);
        u->sil_dur = 0.0f;
    } else {
        fitted_scale(u, u->alt_rate);
        double sd = sil_from(u);
        u->sil_dur = (float)sd;
        if (!x87_jae(sd, 0.0)) u->sil_dur = 0.0f;
    }
}

int32_t unit_set_rate(UnitStage *u, uint32_t r) {
    if (r == 0xffffffffu) {
        u->rate = (uint32_t)x87_ftol((double)u->default_rate * 3.0);
    } else if (r == 0) {
        u->rate = 30;
    } else {
        uint32_t m = (uint32_t)x87_ftol((double)u->default_rate * 3.0);
        if (r > m || r < 30) return (int32_t)0x8000ffff;
        u->rate = r;
    }
    vc_PostMessageA(u->notify_hwnd, u->notify_msg, 2, 0);
    unit_rate_update(u);
    return 0;
}

int32_t unit_reset_rate(UnitStage *u) {
    u->rate = u->default_rate;
    vc_PostMessageA(u->notify_hwnd, u->notify_msg, 2, 0);
    unit_rate_update(u);
    return 0;
}

int32_t unit_set_amp(UnitStage *u, uint32_t v) {
    u->amp = v & 0xffff;
    vc_PostMessageA(u->notify_hwnd, u->notify_msg, 3, 0);
    return 0;
}

LAYOUT(UnitStage, wait_out, 0x2ac);
LAYOUT(UnitStage, cs, 0x2cc);
LAYOUT(UnitStage, out, 0x2f0);

// Blob and TagOut records go into the queues as QItems: the same five fields (in the hook build the
// same bytes; a portable build copies them, its QItem's union being pointer-sized)
static const QItem *blob_items(const Blob *b, uint32_t n, QItem *tmp) {
#ifdef DECOMP_HOOK
    (void)n; (void)tmp;
    return (const QItem *)(const void *)b;
#else
    for (uint32_t i = 0; i < n; i++) {
        memset(&tmp[i], 0, sizeof tmp[i]);
        tmp[i].code = (uint32_t)b[i].type;
        tmp[i].a.value = (uint32_t)b[i]._04;
        tmp[i].v2 = (uint32_t)b[i]._08;
        tmp[i].data = b[i].data;
        tmp[i].bytes = (uint32_t)b[i].bytes;
    }
    return tmp;
#endif
}
static const QItem *tag_item(const TagOut *t, QItem *tmp) {
#ifdef DECOMP_HOOK
    (void)tmp;
    return (const QItem *)(const void *)t;
#else
    memset(tmp, 0, sizeof *tmp);
    tmp->code = (uint32_t)t->code;
    tmp->a.value = (uint32_t)t->value;
    tmp->v2 = (uint32_t)t->_08;
    tmp->data = t->text;
    tmp->bytes = (uint32_t)t->text_bytes;
    return tmp;
#endif
}

void unit_send(UnitStage *u, UnitOutBuf *units) {
    uint32_t n = (uint32_t)units->bytes / 0x50u;
    if (!n) return;
    const UnitOut *src = GP(const UnitOut, units->units);
    Blob *blobs = vc_malloc(n * (uint32_t)sizeof(Blob));
    if (!blobs) return;
    memset(blobs, 0, n * (uint32_t)sizeof(Blob));
    for (int32_t i = 0; i < (int32_t)n; i++) blob_copy80(src + i, blobs + i);
    int drop = locked_get_2cc(u) != 0;
    if (!drop) {
        if (vc_WaitForMultipleObjects(2, u->wait_out, 0, 0xffffffffu) == 0) drop = 1;
        else {
            vc_WaitForMultipleObjects(2, u->wait_out, 0, 0xffffffffu);
#ifdef DECOMP_HOOK
            queue_push_n(GP(Queue, u->out), blob_items(blobs, n, NULL), n);
#else
            QItem *tmp = malloc(n * sizeof(QItem));
            queue_push_n(GP(Queue, u->out), blob_items(blobs, n, tmp), n);
            free(tmp);
#endif
        }
    }
    if (drop)
        for (int32_t i = 0; i < (int32_t)n; i++) vc_free(GP(void, blobs[i].data));
    vc_free(blobs);
}

int32_t unit_phrase(UnitStage *u, PhoneIn *ph, int32_t n) {
    UnitOutBuf st = { 0, 0 };
    int32_t prev = u->sil_phone, i = 0;
    phone_fixups((char *)ph, n);
    unit_select(u, (const char *)ph, n);
    GPTR(PhoneIn) pph;
    GPSET(pph, ph);
    int32_t m = unit_lattice(u, &pph, &n);
    ph = GP(PhoneIn, pph);
    u->last_b = -1;
    u->last_c = -1.0f;
    u->rate_flags = 0;
    u->rate_mode = 0;
    u->alt_rate = u->default_rate;
    PhoneIn *base = ph, *p = ph;
    while (i < n) {
        char c = p->name[0];
        if (c == '\\') {
            TagOut *tag = vc_malloc(sizeof(TagOut));
            if (!tag) return 0;
            memset(tag, 0, sizeof(TagOut));
            tag_parse(p->name, tag);
            if (tag->code == 0x19) {
                unit_set_amp(u, 0xffffffffu);
                unit_reset_rate(u);
                { QItem tq; queue_push(GP(Queue, u->out), tag_item(tag, &tq)); }
            } else if (tag->code == 0x1c) {
                unit_set_amp(u, (uint32_t)tag->value);
            } else if (tag->code == 0x1a) {
                unit_set_rate(u, (uint32_t)tag->value);
            } else {
                { QItem tq; queue_push(GP(Queue, u->out), tag_item(tag, &tq)); }
            }
            vc_free(tag);
            i++;
            p++;
            continue;
        }
        if (c == '#') {
            i++;
            p++;
            continue;
        }
        int32_t j = i + 1, cnt = 1;
        while (j < n && !is_mark(base[j].name[0])) { j++; cnt++; }
        while (j < n && is_mark(base[j].name[0])) j++;
        int next = j < n ? GP(int16_t, u->ids)[cnt] : u->sil_phone;
        unit_prosody(u, p, cnt, &prev, next, &st);
        unit_send(u, &st);
        vc_free(GP(void, st.units));
        UnitRec *recs = GP(UnitRec, u->recs);
        memmove(recs, recs + cnt, (size_t)(uint32_t)(m * 0x4c - cnt * 0x4c));
        int16_t *ids = GP(int16_t, u->ids), *sen = GP(int16_t, u->senones);
        uint32_t bytes = 2u * (uint32_t)(m - cnt);
        memmove(ids, ids + cnt, bytes);
        memmove(sen, sen + cnt, bytes);
        m -= cnt;
        i += cnt;
        p += cnt;
    }
    return 0;
}

int32_t unit_thread_loop(UnitStage *u) {
    QItem it;
    u->wait_in[0] = u->ev_stop;
    u->wait_out[0] = u->ev_stop;
    u->wait_in[1] = GP(Queue, u->in)->ev_nonempty;
    u->wait_out[1] = GP(Queue, u->out)->ev_room;
    for (;;) {
        if (vc_WaitForMultipleObjects(2, u->wait_in, 0, 0xffffffffu) == 0) break;
        if (!queue_pop(GP(Queue, u->in), &it)) continue;
        switch (it.code) {
        case 3:
            if (locked_get_2cc(u) == 0) {
                if (!it.data) continue;
                unit_phrase(u, GP(PhoneIn, it.data), (int32_t)(it.bytes / 100u));
            }
            if (it.data) vc_free(GP(void, it.data));
            continue;
        case 0x1a: unit_set_rate(u, it.a.value); continue;
        case 0x1c: unit_set_amp(u, it.a.value); continue;
        case 0x19:
            unit_reset_rate(u);
            unit_set_amp(u, 0xffffffffu);
            break;
        default: break;
        }
        if (vc_WaitForMultipleObjects(2, u->wait_out, 0, 0xffffffffu) == 0) {
            if (it.data) vc_free(GP(void, it.data));
            continue;
        }
        queue_push(GP(Queue, u->out), &it);
    }
    while (queue_pop(GP(Queue, u->in), &it)) {
        if (it.code == 9 && it.a.obj) vc_com_release(GP(void, it.a.obj));
        if (it.data) vc_free(GP(void, it.data));
    }
    vc_SetEvent(u->ev_done);
    return 0;
}

LAYOUT(AltBucket, right, 0x1c);

int lattice_match(const char *key, const int16_t *sen, int32_t n_rest, int32_t *pos, int32_t i,
                  const AltRules *rules, VecArray *va) {
    (void)n_rest;
    const AltBucket *b = GP(const AltBucket, rules->bucket[(signed char)key[0]]);   // (a signed index, as the original)
    for (int32_t r = *pos; r < b->count; r++) {
        int32_t len;
        if (prefix_cmp(key, GP(const char, GP(GPTR(const char), b->keys)[r]), &len) != 0) continue;
        uint8_t fl = GP(const uint8_t, b->flags)[r];
        if (fl & 0xc) {
            if (array_lacks(sen[i], GP(const uint8_t, b->nleft)[r], GP(const int16_t, GP(GPTR(const int16_t), b->left)[r])))
                continue;
            if (array_lacks(sen[len + i], GP(const uint8_t, b->nright)[r], GP(const int16_t, GP(GPTR(const int16_t), b->right)[r])))
                continue;
        }
        int32_t k = (fl & 1) ? 0 : 1;
        int32_t l2 = (int32_t)strlen(GP(const char, GP(GPTR(const char), b->keys)[r]));
        if (!(fl & 2)) l2--;
        for (; k < l2; k++) vecarray_append(va, i + k, b->first_id + r + 1);
        *pos = r + 1;
        return 1;
    }
    return 0;
}

const int16_t *unit_span_lookup(int32_t x, int32_t n, GPTR(const AltBucket) const *table, GPTR(const char) *tail,
                               int32_t *nunits) {
    int32_t i = 0;
    while (i < n && x >= GP(const AltBucket, table[i + 1])->first_id) i++;
    const AltBucket *b = GP(const AltBucket, table[i]);
    int32_t k = x - b->first_id;
    *tail = GP(GPTR(const char), b->tails)[k];
    *nunits = GP(const int16_t, b->nunits)[k];
    return GP(const int16_t, GP(GPTR(const int16_t), b->units)[k]);
}

static int is_mark_ph(const PhoneIn *p) { return p->name[0] == '#' || p->name[0] == '\\'; }

// remove phone pi of the phrase and unit k of the stage's arrays
static void drop_phone(UnitStage *u, PhoneIn *ph, int32_t *n, int32_t pi, int32_t k) {
    memmove(ph + pi, ph + pi + 1, (size_t)(uint32_t)(*n * 100 - (pi + 1) * 100));
    UnitRec *r = GP(UnitRec, u->recs);
    memmove(r + k, r + k + 1, (size_t)(uint32_t)(u->cap * 0x4c - (k + 1) * 0x4c));
    int16_t *ids = GP(int16_t, u->ids), *sen = GP(int16_t, u->senones);
    memmove(ids + k, ids + k + 1, (size_t)(uint32_t)((u->cap - (k + 1)) * 2));
    memmove(sen + k, sen + k + 1, (size_t)(uint32_t)((u->cap - (k + 1)) * 2));
    (*n)--;
}

static int8_t word_pos(const uint8_t *bounds, int32_t k, int32_t limit) {
    int8_t pos = 0;
    if (bounds[k]) {
        if (k + 1 < limit && bounds[k + 1]) pos = 's';
        else if (bounds[k]) pos = 'b';
    } else if (k + 1 < limit && bounds[k + 1]) {
        pos = 'e';
    }
    return pos;
}

static int32_t senone_of(const UnitStage *u, int32_t cur, int32_t left, int32_t right, int8_t pos) {
    if (u->tree_mode == 1) return senone_lookup(GP(const SenoneTree, u->tree), cur, left, right, pos, 0);
    TreeQuery q;
    memset(&q, 0, sizeof q);   // (the original leaves the unnamed bytes as stack garbage)
    q.cur = (int16_t)cur;
    q.left = (int16_t)left;
    q.right = (int16_t)right;
    q.pos = pos;
    q._2c = 0;
    return tree_query(GP(const SenoneTree, u->tree), &q);
}

int32_t unit_lattice_apply(UnitStage *u, GPTR(PhoneIn) *php, int32_t *n, int32_t m, VecArray *va,
                           uint8_t *bounds) {
    int32_t j = 0, k = 0, pi = 0, delta = 0, changed = 0, prev_changed = 0;
    if (m <= 0) return m;
#define PH GP(PhoneIn, *php)
#define SKIP_MARKS() while (is_mark_ph(PH + pi)) pi++
    const int32_t *res = GP(const int32_t, va->result);
    do {
        SKIP_MARKS();
        int32_t j0 = j;
        if (res[j] == 0) {
            j++, k++, pi++;
            continue;
        }
        GPTR(const char) tailp;
        int32_t nrule;
        const int16_t *units = unit_span_lookup(res[j] - 1, u->senone_base, GP(GPTR(const AltBucket), u->lattice),
                                                &tailp, &nrule);
        const char *tail = GP(const char, tailp);
        int32_t t = 0, k0 = k;
        int32_t tlen = (int32_t)strlen(tail);
        int32_t limit = delta + m;
        for (;;) {
            if (res[j] != res[j0] && t == tlen) break;
            int32_t idx = k - k0;
            const int16_t *up = units + idx;
            if (*up & 0x2000) {             // two phones merge into this one
                drop_phone(u, PH, n, pi, k);
                SKIP_MARKS();
                drop_phone(u, PH, n, pi, k);
                SKIP_MARKS();
                delta -= 2;
                limit -= 2;
                j += 2;
                GP(int16_t, u->senones)[k] = (int16_t)(*up & 0x1fff);
                changed = 1;
            }
            int drop = 0;
            if (idx >= nrule) {
                drop = 1;
            } else if ((*up & 0x8000) && (*up & 0x4000)) {      // the phone is replaced
                GP(int16_t, u->ids)[k] = (int16_t)(int8_t)tail[t];
                GP(UnitRec, u->recs)[k].phone = GP(int16_t, u->ids)[k];
                strcpy(PH[pi].name, GP(const char, named_item(GP(const NamedTable, u->phone_names), (int8_t)tail[t])));
                changed = 1;
                GP(int16_t, u->senones)[k] = (int16_t)(*up & 0x1fff);
                t++;
            } else if (*up & 0x8000) {      // a phone is inserted
                (*n)++;
                delta++;
                limit++;
                GPSET(*php, (PhoneIn *)vc_realloc(GP(void, *php), (size_t)(uint32_t)(*n * 100)));
                memmove(PH + pi + 1, PH + pi, (size_t)(uint32_t)(*n * 100 - (pi + 1) * 100));
                PH[pi].nstates = 0;
                PH[pi].dur = u->sil_phone == (int8_t)tail[t] ? *DLLVAR(const float, 0x636a02bc) : 1.0f;
                strcpy(PH[pi].name, GP(const char, named_item(GP(const NamedTable, u->phone_names), (int8_t)tail[t])));
                if (u->cap < limit) {
                    u->cap = delta + m + 3;
                    GPSET(u->recs, (UnitRec *)vc_realloc(GP(void, u->recs), (size_t)(uint32_t)(u->cap * 0x4c)));
                    GPSET(u->ids, (int16_t *)vc_realloc(GP(void, u->ids), (size_t)(uint32_t)(u->cap * 2)));
                    GPSET(u->senones, (int16_t *)vc_realloc(GP(void, u->senones), (size_t)(uint32_t)(u->cap * 2)));
                }
                UnitRec *r = GP(UnitRec, u->recs);
                memmove(r + k + 1, r + k, (size_t)(uint32_t)(u->cap * 0x4c - (k + 1) * 0x4c));
                int16_t *ids = GP(int16_t, u->ids), *sen = GP(int16_t, u->senones);
                memmove(ids + k + 1, ids + k, (size_t)(uint32_t)((u->cap - (k + 1)) * 2));
                memmove(sen + k + 1, sen + k, (size_t)(uint32_t)((u->cap - (k + 1)) * 2));
                ids[k] = (int16_t)(int8_t)tail[t];
                changed = 1;
                r[k].phone = ids[k];
                t++;
                j--;
                sen[k] = (int16_t)(*up & 0x1fff);
            } else if (!(*up & 0x4000)) {  // the unit is swapped
                GP(int16_t, u->senones)[k] = (int16_t)(*up & 0x1fff);
            } else {
                drop = 1;
            }
            if (drop) {                     // the phone is deleted
                drop_phone(u, PH, n, pi, k);
                SKIP_MARKS();
                delta--;
                limit--;
                GP(int16_t, u->senones)[k] = (int16_t)(*up & 0x1fff);   // (read even past the rule's units)
                changed = 1;
                j++;
            }
            if (prev_changed || changed) {  // the senones around an edit follow the new context
                UnitRec *r = GP(UnitRec, u->recs);
                int32_t left = k == 0 ? u->sil_phone : r[k - 1].phone;
                int32_t right = (limit == k && t == tlen) ? u->sil_phone : r[k + 1].phone;
                r[k].senone = senone_of(u, r[k].phone, left, right, word_pos(bounds, k, limit));
                if (changed && k != 0) {
                    int8_t pos = 0;
                    if (bounds[k - 1]) pos = bounds[k] ? 's' : 'b';
                    else if (bounds[k]) pos = 'e';
                    left = k - 1 == 0 ? u->sil_phone : r[k - 2].phone;
                    right = (limit == k - 1 && t == tlen) ? u->sil_phone : r[k].phone;
                    r[k - 1].senone = senone_of(u, r[k - 1].phone, left, right, pos);
                }
            }
            prev_changed = changed;
            changed = 0;
            j++, k++, pi++;
            SKIP_MARKS();
        }
    } while (j < m);
#undef SKIP_MARKS
#undef PH
    return m + delta;
}

int32_t unit_lattice(UnitStage *u, GPTR(PhoneIn) *ph, int32_t *n) {
    int32_t result;
    VecArray *va = NULL;
    uint8_t *ids8 = vc_calloc(1, (size_t)(uint32_t)*n);
    uint8_t *bounds = vc_calloc((size_t)(uint32_t)*n, 1);
    if (!ids8 || !bounds) {
        // (the original returns an uninitialised local here)
        result = 0;
        goto done;
    }
    int32_t m = 0;
    uint8_t *bp = bounds;
    const PhoneIn *p = GP(const PhoneIn, *ph);
    for (int32_t i = 0; i < *n; i++) {
        char c = p[i].name[0];
        if (c == '#' || c == '\\') *bp = 1;
        else {
            ids8[bp - bounds] = (uint8_t)GP(int16_t, u->ids)[m];
            m++;
            bp++;
        }
    }
    if (!u->lattice || !m) {
        vc_free(ids8);
        vc_free(bounds);
        return m;
    }
    result = m;
    VecArray *v = vc_malloc(sizeof(VecArray));
    va = v ? vecarray_ctor(v) : NULL;
    vecarray_init(va, m, 10, -1);
    for (int32_t i = 0; i < m; i++) {
        int32_t pos = 0;
        while (lattice_match((const char *)ids8 + i, GP(int16_t, u->senones) + i, m - i, &pos, i,
                             GP(const AltRules, u->lattice), va))
            ;
    }
    lattice_viterbi(va, DLLCODE(0x63685d00));
    result = unit_lattice_apply(u, ph, n, result, va, bounds);
done:
    if (ids8) vc_free(ids8);
    if (bounds) vc_free(bounds);
    if (va) {
        vecarray_destroy(va);
        vc_free(va);
    }
    return result;
}

int32_t unit_get_rate(const UnitStage *u, uint32_t *r) {
    *r = u->rate;
    return 0;
}
