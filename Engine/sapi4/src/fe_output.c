#include "fe_output.h"
#include "fe_vm.h"
#include "fe_phrase.h"
#include "fe_word.h"
#include "crt_vc.h"
#include <stdio.h>
#include <string.h>

#define DIV10(v) ((int32_t)(v) / 10)


static void set_tapes(uint8_t *w, uint32_t sel, uint32_t tape5, uint32_t wglob) {
    static const uint32_t off[5] = { 0x585, 0xb05, 0x1085, 0x1605, 0x438e };
    for (int q = 0; q < 5; q++) {
        GPSET(*DLLPTR(char, rule_vm[1].tape[q]), (char *)w + off[q]);     // (B and C share tapes 0-4)
        GPSET(*DLLPTR(char, sel + 4u * (uint32_t)q), (char *)w + off[q] - 1);
    }
    GPSET(*DLLPTR(char, tape5), (char *)w + 5);
    GPSET(*DLLPTR(uint8_t, wglob), w);
}

int32_t fe_c_run(uint8_t *w) {
    *DLLVAR(int16_t, 0x63721f78) = 0x501;
    set_tapes(w, 0x637395c0, 0x637395b4, 0x63739624);
    for (int i = 0; i < 0x500; i++) { W16(w, 0x1b8c + 2 * i) = 0; W16(w, 0x258c + 2 * i) = 0; }
    int16_t s = (int16_t)(W16(w, 0) - 1), e = W16(w, 2);
    W16(w, 0x580e) = 0;
    W16(w, 0x438c) = 0;
    for (int32_t i = s; i < e; i++) {
        w[0xb05 + i] = w[0x585 + i];
        w[0x438e + i] = ' ';
        w[0x1605 + i] = ' ';
        w[0x1085 + i] = ' ';
    }
    *DLLVAR(int32_t, 0x63739744) = 0;
    vm_proc(2, 50, (int16_t *)(void *)w, (int16_t *)(void *)(w + 2), 0);
    return *DLLVAR(int32_t, 0x63739744) ? -1 : 0;
}

int32_t fe_b_run(uint8_t *w) {
    *DLLVAR(int32_t, 0x63739564) = 0;
    *DLLVAR(int16_t, 0x63721f28) = 0x501;
    set_tapes(w, 0x637393e0, 0x637393d0, 0x63739444);
    int16_t *l = (int16_t *)(void *)w, *n = (int16_t *)(void *)(w + 2);
    vm_proc(1, 16, l, n, 0);
    vm_proc(1, 6, l, n, 0);
    vm_proc(1, 3, l, n, 0);
    return *DLLVAR(int32_t, 0x63739564) ? -1 : 0;
}

int32_t fe_prepare(uint8_t *w) {
    uint8_t st = w[0xb4bc];
    if (st == 4) return 0;
    if (st != 1) return -0x64;
    if (fe_c_run(w) || fe_b_run(w)) return -0x65;
    int16_t ax = (int16_t)(W16(w, 0) - 1), dx = W16(w, 2);
    while (ax < dx) {
        uint8_t c = w[0x585 + ax];
        if (c != '#' && c != '~' && c != ' ') break;
        ax++;
    }
    if (ax == dx) return -0x65;
    w[0xb4bc] = 2;
    return 0;
}

static uint8_t *wobj(int32_t idx) {
    if (idx < 1 || idx > 0x80) return NULL;
    GPTR(uint8_t) p = DLLPTR(uint8_t, FE_WOBJS)[idx];
    return p ? GP(uint8_t, p) : NULL;
}
#define FE_READY (*DLLVAR(int32_t, FE_MAGIC) == (int32_t)0xffff9bad)

int32_t fe_word_prepare(int32_t idx) {
    if (!FE_READY) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    int32_t r = fe_prepare(w);
    if (r == -0x64) return -0x78;
    if (r == -1) return -0x79;
    for (int i = 0; i < 0x500; i++) { W16(w, 0x1b8c + 2 * i) = 0; W16(w, 0x258c + 2 * i) = 0; }
    W16(w, 0x580e) = 0;
    W16(w, 0x438c) = 0;
    w[0xb4bc] = 2;
    return 0;
}

int32_t fe_word_format(int32_t idx, GPTR(char) *out) {
    if (!FE_READY) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    if (ph_run(w) == (int32_t)0xffffff38) return (int32_t)0xffffff74;
    int32_t n = W16(w, 0x580e);
    char *buf = vc_malloc((size_t)(W16(w, 2) * 7 - W16(w, 0) * 7 + n * 10 + 0xd));
    GPSET(*out, buf);
    if (!buf) return -0x64;
    // the pitch targets, as (time in 10 ms steps so far, frequency) pairs in the tag pair area
    uint32_t acc = 0;
    for (int32_t k = 0; k < n; k++) {
        W16(w, 0x2f8c + 8 * k + 4) = W16(w, 0x4910 + 6 * k);
        W32(w, 0x2f8c + 8 * k) = (uint16_t)acc;
        acc += (uint32_t)DIV10(*(uint16_t *)(void *)(w + 0x4912 + 6 * k));
    }
    int32_t s = W16(w, 0);
    int32_t len = sprintf(buf, "%c/%c[%d", 0x1b, w[0x584 + s], (int)W16(w, 0x1b8a + 2 * s));
    for (int32_t k = 0; k < n; k++)
        len += sprintf(buf + strlen(buf), "(%u,%d)", (unsigned)(uint16_t)W32(w, 0x2f8c + 8 * k), (int)W16(w, 0x2f8c + 8 * k + 4));
    len++;
    strcat(buf, "]");
    for (int32_t p = s + 1; p <= W16(w, 2); p++) {
        uint8_t c = w[0x584 + p];
        if (c == '-') len += sprintf(buf + strlen(buf), "%c", 0x2d);
        else len += sprintf(buf + strlen(buf), "%c[%d]", c, (int)W16(w, 0x1b8a + 2 * p));
    }
    len += sprintf(buf + strlen(buf), "%c/%cE ", 0x1b, 0x1b);
    return len;
}

int32_t fe_event_count(uint8_t *w) {
    if (w && W16(w, 0xb4ba) == (int16_t)0x9b39) return W32(w, 0xb4b4);
    return -1;
}

int32_t fe_word_event_count(int32_t idx) {
    if (!FE_READY) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    return fe_event_count(w);
}

int32_t fe_events(uint8_t *w, int32_t *dst) {
    if (!w || W16(w, 0xb4ba) != (int16_t)0x9b39) return -1;
    int32_t n = 0;
    for (GPTR(EvNode) e = WPTR(w, 0xb4ac); e; n++) {
        EvNode *x = GP(EvNode, e);
        int32_t *d = dst + 6 * n;
        memcpy(d, x, 24);
        d[3] *= 10;
        d[4] *= 10;
        e = x->next;
    }
    return n;
}

int32_t fe_word_events(int32_t idx, int32_t *dst) {
    if (!FE_READY) return -0x66;
    uint8_t *w = wobj(idx);
    if (!w) return -0x67;
    return fe_events(w, dst);
}
