#include "fe_word.h"

#define TAPE() ((const uint8_t *)GP(char, *DLLPTR(char, WORD_TAPE)))
#define CLS(c) DLLVAR(const uint8_t, 0x636b6588)[(uint8_t)(c)]
#define PAIR(a, b) DLLVAR(const uint16_t, 0x636b6688)[CLS(a) * 0x2a + CLS(b)]

void word_stop_weight(uint8_t *w, int16_t i, int16_t c) {
    if (c > 0xff) c = 0xff;
    else if (c < 0) c = 0;
    uint8_t ch = TAPE()[i - 1];
    int stop = ch == 0x3f || ch == 0x62 || ch == 0x64 || ch == 0x67 || ch == 0x6b || ch == 0x70 || ch == 0x74;
    W8(w, 0xa317 + i) = stop ? (uint8_t)c : 0;
}

// the letters of the table entry at `off` against tape A from `key`: the first mismatching index + 1,
// or len + 1 if all len (+1: the original compares one past) match
static int16_t cluster_cmp(const uint8_t *s, int16_t off, const uint8_t *key, int16_t len) {
    int16_t k = 1;
    if (s[off] == key[0])
        while (k <= len) {
            k++;
            if (s[off + k - 1] != key[k - 1]) break;
        }
    return k;
}

int16_t word_cluster_find(int16_t lo, int16_t hi, int16_t len, int16_t pos) {
    pos--;
    if (lo <= 0) return 0;
    const uint8_t *key = TAPE() + pos;
    if (lo < hi) {
        const uint8_t *s = GP(uint8_t, *DLLPTR(uint8_t, 0x636b8708 + 8u * (uint32_t)len));
        do {
            int32_t mid = ((int32_t)lo + hi) / 2;
            int16_t off = (int16_t)(len * (mid - 1));
            int16_t k = cluster_cmp(s, off, key, len);
            if (k > len) lo = hi = (int16_t)mid;
            else if (s[off + k - 1] > key[k - 1]) hi = (int16_t)(mid - 1);
            else lo = (int16_t)(mid + 1);
        } while (lo < hi);
    }
    if (lo != hi) return 0;
    const uint8_t *s = GP(uint8_t, *DLLPTR(uint8_t, 0x636b8708 + 8u * (uint32_t)len));
    int16_t off = (int16_t)((lo - 1) * len);
    if (cluster_cmp(s, off, key, len) <= len) return 0;
    return GP(int16_t, *DLLPTR(int16_t, 0x636b870c + 8u * (uint32_t)len))[lo - 1];
}

int16_t word_cluster(int16_t *pp, int16_t end) {
    const uint8_t *t = TAPE();
    int16_t dx = *pp;
    uint8_t flag = 0, found = 0;
    uint16_t saved = 0;
    int16_t si, di, r = 0;
    uint8_t c = t[dx];
    if (c != 0x2d) {
        si = (int16_t)PAIR(t[dx - 1], c);
        if (si & 0x2000) { flag = 1; saved = (uint16_t)si; }
        si = (int16_t)(si & 0x9fff);
        di = (int16_t)(dx + 2);
    } else {
        si = (int16_t)PAIR(t[dx - 1], t[dx + 1]);
        if (si & 0x4000) { flag = 2; saved = (uint16_t)si; }
        si = (int16_t)(si & 0x9fff);
        if (si <= 0xa6 && si > 0) si = (int16_t)(si + 0x561);
        di = (int16_t)(dx + 3);
    }
    const int16_t *t4 = DLLVAR(const int16_t, 0x636b744e);
    int16_t base = t4[si];
    int16_t n = (int16_t)(t4[si + 1] - base);
    if (n > 0 && si > 0) {
        int16_t bx = n;
        do {
            if (found) break;
            if ((int32_t)end - di + 1 >= bx) {
                int32_t idx = bx + base;
                r = word_cluster_find(DLLVAR(const int16_t, 0x636b8060)[idx - 2], DLLVAR(const int16_t, 0x636b8168)[idx - 2], bx, di);
                found = r > 0;
            }
            bx--;
        } while (bx > 0);
        if (found) { si = r; *pp = (int16_t)(bx + di); }
        else *pp = (int16_t)(di - 1);
    } else *pp = (int16_t)(di - 1);
    if (!found && flag) {
        uint16_t ax;
        if (flag == 2) ax = (saved & 0x2000) ? 0 : (uint16_t)(saved & ~0x4000u);
        else {
            if (saved & 0x4000) saved = 0;
            ax = (uint16_t)(saved & ~0x2000u);
            if ((int16_t)ax <= 0xa6 && (int16_t)ax > 0) ax = (uint16_t)(ax + 0x561);
        }
        if (ax) return (int16_t)ax;
        return (int16_t)(PAIR(0x23, 0x74) & 0x9fff);
    }
    if (si == 0) si = (int16_t)(PAIR(0x23, 0x74) & 0x9fff);
    return si;
}

uint8_t word_durations(uint8_t *w) {
    uint8_t ok = 1;
    int16_t s = W16(w, 0), e = W16(w, 2);
    GPSET(*DLLPTR(char, WORD_TAPE), (char *)w + 0x585);
    const uint8_t *t = TAPE();
    for (int32_t i = s - 1; i < e; i++) {
        W16(w, 0xa938 + 2 * i) = 0;
        W16(w, 0x9918 + 2 * i) = 0;
        W8(w, 0xa318 + i) = 0;
    }
    int16_t p = s;
    while (p < e) {
        int16_t q = p;
        int16_t r = word_cluster(&p, e);
        W16(w, 0xa936 + 2 * q) = r;
        int16_t len = (int16_t)(p - q - (t[q] == 0x2d ? 2 : 1));
        if (r == 0) { ok = 0; continue; }
        int32_t k = 1;
        for (int16_t i = q; i <= p; i++) {
            if (t[i - 1] == 0x2d) continue;
            int16_t c;
            int16_t v = W16(w, 0xa936 + 2 * q);
            if (len != 0) {
                const uint8_t *tab = GP(uint8_t, *DLLPTR(uint8_t, 0x63722078 + 4u * (uint32_t)(int32_t)len));
                int32_t idx = (v - DLLVAR(const int16_t, 0x636b8736)[len] - 1) * (len + 2);
                c = tab[idx + (int16_t)k - 1];
            } else {
                const uint8_t *tab = GP(uint8_t, *DLLPTR(uint8_t, 0x63722078));
                c = tab[v * 2 + (int16_t)k - 3];
            }
            W16(w, 0x9916 + 2 * i) = (int16_t)(W16(w, 0x9916 + 2 * i) + c);
            word_stop_weight(w, i, c);
            k++;
        }
    }
    return ok;
}
