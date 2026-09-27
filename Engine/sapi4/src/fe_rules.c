#include "fe_rules.h"

#define TAPE(addr) GP(char, *DLLPTR(char, addr))

int tapes_graphic(int16_t lo, int16_t hi) {
    for (int16_t i = lo; i <= hi; i++) {
        uint8_t ch = (uint8_t)TAPE(TAPE_A)[i];
        if (ch < 0x21 || ch > 0xfe) return 0;
    }
    return 1;
}

int16_t tapes_pad(const int16_t *ends, int16_t a) {
    int16_t m = a;
    for (int k = 0; k < 4; k++)
        if (m < ends[k]) m = ends[k];
    static const uint32_t tapes[4] = { TAPE_A, TAPE_B, TAPE_C, TAPE_D };
    for (int k = 0; k < 4; k++)
        for (int16_t i = (int16_t)(ends[k] + 1); i <= m; i++) TAPE(tapes[k])[i] = '~';
    return m;
}

uint8_t rule_bsearch(const uint8_t *table, uint32_t base, uint8_t lo, uint8_t hi, uint8_t width) {
    const uint8_t *key = DLLVAR(const uint8_t, RULE_KEY);
    while (hi > lo) {
        uint8_t mid = (uint8_t)(lo + (((int)hi - (int)lo) >> 1));
        uint16_t pos = (uint16_t)(width * mid + base);
        uint8_t k = 0;
        if (key[0] == table[pos]) {
            uint8_t last = (uint8_t)(width - 1);
            while (k < last) {
                pos++;
                k++;
                if (key[k] != table[pos]) break;
            }
        }
        if (key[k] > table[pos]) lo = (uint8_t)(mid + 1);
        else hi = mid;
    }
    uint16_t at = (uint16_t)(width * lo + base);
    for (uint8_t k = 0; k < width; k++)
        if (key[k] != table[at + k]) return 0xff;
    return lo;
}
