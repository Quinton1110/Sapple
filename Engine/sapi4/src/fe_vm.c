#include "fe_vm.h"
#include "crt_vc.h"

const RuleVM rule_vm[3] = {
    { { 0x63739268, 0x63739278, 0x63738ca8, 0x63739204, 0x637391f8, 0x63738cf0 }, 0x63721cc8, 0x637392e0 },
    { { 0x63739e7c, 0x63739e6c, 0x63739e78, 0x63739e70, 0x63739e74, 0x637393d0 }, 0x63721f28, 0x637394a8 },
    { { 0x63739e7c, 0x63739e6c, 0x63739e78, 0x63739e70, 0x63739e74, 0x637395b4 }, 0x63721f78, 0x63739688 },
};

#define TAPE(vm, k) GP(char, *DLLPTR(char, (vm)->tape[k]))
#define TOP(vm) (*DLLVAR(int16_t, (vm)->top))
#define PC(vm) (*DLLPTR(uint8_t, (vm)->pc))

void vm_shift_right(const RuleVM *vm, int16_t lo, int16_t hi) {
    if (hi >= lo)
        for (int32_t i = hi, n = hi - lo + 1; n; n--, i--)
            for (int k = 0; k < 6; k++) TAPE(vm, k)[i] = TAPE(vm, k)[i - 1];
    for (int k = 0; k < 5; k++) {
        TAPE(vm, k)[hi + 1] = 0x0e;
        TAPE(vm, k)[lo - 1] = 0x0e;
    }
    TAPE(vm, 5)[hi + 1] = 0;
    TAPE(vm, 5)[lo - 1] = 0;
}

uint32_t vm_shift_left(const RuleVM *vm, int16_t lo, int16_t hi, uint32_t prev_eax) {
    if (lo > hi) return (prev_eax & 0xffff0000u) | (uint16_t)lo;
    int32_t i = lo;
    for (int32_t n = hi - lo + 1; n; n--, i++)
        for (int k = 0; k < 6; k++) TAPE(vm, k)[i - 2] = TAPE(vm, k)[i - 1];
    return (uint32_t)i;
}

void vm_pop_range(const RuleVM *vm, int16_t a, int16_t b) {
    int32_t stop = a - 1;
    for (int32_t i = b; i >= stop; b--, i = b) {
        TOP(vm)--;
        for (int k = 0; k < 6; k++) TAPE(vm, k)[TOP(vm) - 1] = TAPE(vm, k)[i];
        TAPE(vm, 5)[i] = 0;
    }
}

int32_t vm_push_until(const RuleVM *vm, int32_t s) {
    do {
        s++;
        int16_t i = (int16_t)s;
        for (int k = 0; k < 6; k++) TAPE(vm, k)[i - 1] = TAPE(vm, k)[TOP(vm) - 1];
        TOP(vm)++;
    } while (TAPE(vm, 0)[(int16_t)s - 1] != 0x0e);
    return s - 1;
}

uint32_t vm_skip_block(const RuleVM *vm, uint8_t tok, uint8_t end) {
    uint8_t *p = GP(uint8_t, PC(vm));
    uint8_t d = p[0], c = p[1];
    for (;;) {
        if (d == tok && c == end) return ((uint32_t)(uintptr_t)GRAW(PC(vm)) & ~0xffu) | 1u;
        if ((d == tok && c == 1) || (tok == 0xf5 && d == tok && c != 0)) {
            GPSET(PC(vm), p + 1);
            vm_skip_block(vm, tok, 0);
            p = GP(uint8_t, PC(vm)) + 2;
        }
        p++;
        GPSET(PC(vm), p);
        c = p[0];
        if (c == tok && p[1] == 0) return ((uint32_t)GRAW(PC(vm)) & ~0xffu);
        d = c;
        c = p[1];
    }
}

int32_t vm_move(const RuleVM *vm, int32_t a, int32_t b, int32_t d) {
    if ((int16_t)d > 0) {
        int32_t stop = (int16_t)a - 1;
        int32_t i = (int16_t)b;
        if (i >= stop) {
            int32_t dst = d + b + 1, cnt = b;
            do {
                int16_t k0 = (int16_t)dst;
                for (int k = 0; k < 6; k++) TAPE(vm, k)[k0 - 1] = TAPE(vm, k)[i];
                TAPE(vm, 5)[i] = 0;
                dst--;
                cnt--;
                i = (int16_t)cnt;
            } while (i >= stop);
        }
    } else {
        int32_t start = a - 1;
        if (!((int16_t)start > (int16_t)b)) {
            int32_t n = (int16_t)b - (int16_t)start + 1;
            int32_t i = (int16_t)start, dst = start + d + 1;
            for (; n; n--, dst++, i++) {
                int16_t k0 = (int16_t)dst;
                for (int k = 0; k < 5; k++) TAPE(vm, k)[k0 - 1] = TAPE(vm, k)[i];
                TAPE(vm, 5)[k0 - 1] = (char)(TAPE(vm, 5)[k0 - 1] + TAPE(vm, 5)[i]);
                TAPE(vm, 5)[i] = 0;
            }
        }
    }
    return b + d;
}

const CondVM cond_vm[3] = {
    { 0x637392e0, 0x63739284, 0x6373927c, 0x6373933c, 0x63739204, 0x636a1cd8, 0x636a0750, 0x63738ce0,
      0x636a1ed6, 0x636a1d7a, 0x636a07f2, 0x63721ccc, 1 },
    { 0x637394a8, 0x6373944c, 0x63739448, 0x63739504, 0x637393dc, 0x636a4eb8, 0x636a3238, 0x637393c4,
      0x636a522e, 0x636a4f5a, 0x636a32da, 0x63721f2c, 0 },
    { 0x63739688, 0x6373962c, 0x63739628, 0x637396e4, 0x637395bc, 0x636ab308, 0x636a7c98, 0x63739590,
      0x636abea6, 0x636ab3aa, 0x636a7d3a, 0x63721f7c, 0 },
};

uint32_t vm_cond(const CondVM *vm, uint8_t end, int32_t pos) {
    GPTR(uint8_t) *PCv = DLLPTR(uint8_t, vm->pc);
    uint8_t *pc = GP(uint8_t, *PCv);
    if (vm->pos16) pos = (int16_t)pos;
#define RET(r) return ((uint32_t)GRAW(*PCv) & ~0xffu) | (r)   // (pc == *PCv at every return)
    if (pc[0] == end) RET(1u);
    GPTR(char) saved = *DLLPTR(char, vm->saved);
    GPTR(char) cur = *DLLPTR(char, vm->cur);
    uint8_t ch = *DLLVAR(uint8_t, vm->ch);
    const uint8_t *vars = DLLVAR(const uint8_t, vm->vars);
    for (;;) {
        uint8_t op = pc[0], arg = pc[1];
        switch (op) {
        case 0xef: case 0xf1:
            saved = cur;
            *DLLPTR(char, vm->saved) = saved;
            cur = DLLPTR(char, vm->tapes)[arg];
            *DLLPTR(char, vm->cur) = cur;
            ch = (uint8_t)GP(char, cur)[pos];
            *DLLVAR(uint8_t, vm->ch) = ch;
            break;
        case 0xee: case 0xf0:
            cur = saved;
            *DLLPTR(char, vm->cur) = cur;
            ch = (uint8_t)GP(char, saved)[pos];
            *DLLVAR(uint8_t, vm->ch) = ch;
            break;
        case 0xf2: if (arg == ch) RET(0u); break;
        case 0xf3: if (arg != ch) RET(0u); break;
        case 0xf9: if (DLLVAR(const uint8_t, vm->t1)[arg * 0xa2 + ch]) RET(0u); break;
        case 0xfa: if (!DLLVAR(const uint8_t, vm->t1)[arg * 0xa2 + ch]) RET(0u); break;
        case 0xfb: if (DLLVAR(const uint8_t, vm->t2)[arg * 0xa2 + ch]) RET(0u); break;
        case 0xfc: if (!DLLVAR(const uint8_t, vm->t2)[arg * 0xa2 + ch]) RET(0u); break;
        case 0xfd: {
            uint8_t v = vars[arg];
            if (v) {
                if (ch != v) RET(0u);
                break;
            }
            const uint8_t *cl = DLLVAR(const uint8_t, vm->cls) + arg * 2;
            if (cl[0] == 4) {
                if (!DLLVAR(const uint8_t, vm->t4)[cl[1] * 0xa2 + ch]) RET(0u);
            } else if (cl[0] == 5) {
                if (!DLLVAR(const uint8_t, vm->t3)[cl[1] * 0xa2 + ch]) RET(0u);
            }
            *DLLVAR(int16_t, vm->bound) = 1;
            DLLVAR(uint8_t, vm->vars)[arg] = ch;
            break;
        }
        case 0xea: {
            uint8_t v = vars[arg];
            if (v && ch == v) RET(0u);
            break;
        }
        default:
            for (;;) {}   // (the original spins here; see the header)
        }
        pc += 2;
        GPSET(*PCv, pc);
        if (pc[0] == end) RET(1u);
    }
#undef RET
}

const RunVM run_vm[3] = {
    { 0x63738ce8, 0x637392e0, 0x6373924c, 0x63738cb8, 0x63738cd0, 0x63738cbc, 0x63739250, 0x63739340,
      0x63739288, 0x637392e8, 0x63739220, 0x63739370, 0x6373927c, 0x6373939c, 0x63739200, 0x63721ccc, 0x63738ce0, 8 },
    { 0x637393cc, 0x637394a8, 0x63739424, 0x637393ac, 0x637393b8, 0x637393b0, 0x63739428, 0x63739508,
      0x63739450, 0x637394b0, 0x637393f8, 0x63739538, 0x63739448, 0x63739564, 0x637393d8, 0x63721f2c, 0x637393c4, 7 },
    { 0x637395b0, 0x63739688, 0x63739604, 0x63739574, 0x63739580, 0x63739578, 0x63739608, 0x637396e8,
      0x63739630, 0x63739690, 0x637395d8, 0x63739718, 0x63739628, 0x63739744, 0x637395bc, 0x63721f7c, 0x63739590, 30 },
};

// the executor's own saved-tape global (per instance); the rest are RunVM / CondVM / MatchVM globals
static const uint32_t exec_saved[3] = { 0x637392dc, 0x637394a4, 0x63739684 };

uint8_t vm_exec(int inst) {
    const RunVM *rv = &run_vm[inst];
    const CondVM *cv = &cond_vm[inst];
    const MatchVM *mv = &match_vm[inst];
    GPTR(uint8_t) *PCv = DLLPTR(uint8_t, rv->pc);
    int16_t *P = DLLVAR(int16_t, rv->pos), *A = DLLVAR(int16_t, rv->pos0), *S = DLLVAR(int16_t, rv->step);
    int16_t *nch = DLLVAR(int16_t, rv->nch), *last = DLLVAR(int16_t, mv->last);
    uint8_t *chg = DLLVAR(uint8_t, cv->ch), *tries = DLLVAR(uint8_t, rv->tries);
    GPTR(char) *T = DLLPTR(char, rv->cur);
    uint8_t *pc = GP(uint8_t, *PCv);
    uint8_t bl = 1;
    int32_t cnt = 0;
#define TC(i) ((uint8_t)GP(char, *T)[(int16_t)(i)])
#define SETPC(p) GPSET(*PCv, (p))
    // skip filler in the current direction (when enabled), then read the character at P
#define SKIPREAD() do { int16_t q_ = *P; if (*A) while (TC(q_) == 0x7e) q_ = (int16_t)(q_ + *A); \
                        *P = q_; *chg = TC(q_); } while (0)
#define CLS(t, a, c) DLLVAR(const uint8_t, cv->t)[(a) * 0xa2 + (c)]
    uint8_t op = pc[0];
    switch (op) {
    case 0xfd: {                            // variable: compare, or bind if its class allows
        SKIPREAD();
        uint8_t ch = *chg;
        SETPC(++pc);
        uint8_t v = pc[0], b = DLLVAR(uint8_t, cv->vars)[v];
        if (b) { if (ch != b) bl = 0; break; }
        const uint8_t *cl = DLLVAR(const uint8_t, cv->cls) + v * 2;
        if (cl[0] == 4) { if (!CLS(t4, cl[1], ch)) { bl = 0; break; } }
        else if (cl[0] == 5) { if (!CLS(t3, cl[1], ch)) { bl = 0; break; } }
        else break;                         // (other kinds: accepted without binding)
        *DLLVAR(int16_t, cv->bound) = 1;
        DLLVAR(uint8_t, cv->vars)[v] = ch;
        break;
    }
    case 0xea: {                            // not the bound variable
        SKIPREAD();
        uint8_t ch = *chg;
        SETPC(++pc);
        uint8_t b = DLLVAR(uint8_t, cv->vars)[pc[0]];
        if (b && ch == b) bl = 0;
        break;
    }
    case 0xfc: case 0xfb: {                 // class t2: the table byte itself / its negation
        SKIPREAD();
        uint8_t ch = *chg;
        SETPC(++pc);
        if (ch == 0x0e) { bl = 0; break; }
        uint8_t t = CLS(t2, pc[0], ch);
        bl = op == 0xfc ? t : (uint8_t)(t == 0);
        break;
    }
    case 0xfa: case 0xf9: {                 // class t1 (pc moves after the end-of-tape test)
        SKIPREAD();
        uint8_t ch = *chg;
        if (ch == 0x0e) { bl = 0; break; }
        SETPC(++pc);
        uint8_t t = CLS(t1, pc[0], ch);
        bl = op == 0xfa ? t : (uint8_t)(t == 0);
        break;
    }
    case 0xf3: case 0xf2: {                 // literal / not literal
        SKIPREAD();
        uint8_t ch = *chg;
        SETPC(++pc);
        if (ch == 0x0e) { bl = 0; break; }
        if ((pc[0] == ch) != (op == 0xf3)) bl = 0;
        break;
    }
    case 0xef: case 0xf1:                   // switch to tape pc[1]
        *DLLPTR(char, exec_saved[inst]) = *T;
        SETPC(++pc);
        *T = DLLPTR(char, cv->tapes)[pc[0]];
        *P = (int16_t)(*P - *S);
        *A = 0;
        return 1;
    case 0xee: case 0xf0:                   // back to the previous tape
        SETPC(++pc);
        *T = *DLLPTR(char, exec_saved[inst]);
        *P = (int16_t)(*P - *S);
        *A = 0;
        return 1;
    case 0xec: {                            // skip filler (always), stay before the next character
        int16_t q = *P, s = *S;
        while (TC(q) == 0x7e) { q = (int16_t)(q + s); *P = q; }
        *chg = TC(q);
        *P = (int16_t)(q - s);
        *A = s;
        return 1;
    }
    case 0xf6: {                            // alternatives: pc[1] = which one (1: push a choice point)
        SETPC(++pc);
        uint8_t c = pc[0];
        if (c == 1) {
            int16_t n = ++*nch;
            DLLVAR(int16_t, rv->cpos0)[n] = *A;
            DLLVAR(int16_t, rv->ckind)[n] = 0xf6;
            DLLPTR(char, rv->ctape)[n] = *T;
            DLLVAR(int16_t, rv->cpos)[n] = *P;
            tries[n] = c;
            GPSET(DLLPTR(uint8_t, rv->cpc)[n], pc + 1);
        } else if (c > 1) {
            vm_skip_block(&rule_vm[inst], 0xf6, 0);
            SETPC(GP(uint8_t, *PCv) + 1);
        }
        *P = (int16_t)(*P - *S);
        return 1;
    }
    case 0xf5: {                            // optional part: push a retry-once choice point
        SETPC(++pc);
        if (pc[0] != 0) {
            int16_t n = ++*nch;
            DLLVAR(int16_t, rv->cpos0)[n] = *A;
            DLLPTR(char, rv->ctape)[n] = *T;
            DLLVAR(int16_t, rv->ckind)[n] = 0xf5;
            DLLVAR(int16_t, rv->cpos)[n] = *P;
            GPSET(DLLPTR(uint8_t, rv->cpc)[n], pc + 1);
            vm_skip_block(&rule_vm[inst], 0xf5, 0);
            SETPC(GP(uint8_t, *PCv) + 1);
        }
        *P = (int16_t)(*P - *S);
        return 1;
    }
    case 0xf7: {                            // bounded repetition of a condition, with backtracking
        pc += 3;
        SETPC(pc);
        if (*DLLVAR(uint8_t, rv->flag) == 0) {
            // resumed after backtracking: one more condition test at P
            SKIPREAD();
            int16_t q = *P;
            *DLLVAR(uint8_t, rv->flag) = 1;
            if (TC(q) == 0x0e) bl = 0;
            else bl = (uint8_t)vm_cond(cv, 0xf7, q);
            int16_t n = *nch, s = *S;
            DLLVAR(int16_t, rv->cpos)[n] = (int16_t)(*P + s);
            DLLVAR(int16_t, rv->cpos0)[n] = s;
            if (!bl) (*nch)--;
            *A = s;
            return bl;
        }
        int16_t n = ++*nch;
        uint8_t *start = pc - 3;
        tries[n] = 0;
        DLLVAR(int16_t, rv->ckind)[n] = 0xf7;
        DLLPTR(char, rv->ctape)[n] = *T;
        GPSET(DLLPTR(uint8_t, rv->cpc)[n], start);
        if (start[1] > 0) {
            do {
                if (!bl) break;
                SKIPREAD();
                int16_t q = *P;
                tries[n]++;
                SETPC(GP(uint8_t, DLLPTR(uint8_t, rv->cpc)[n]) + 3);
                if (TC(q) == 0x0e) bl = 0;
                else bl = (uint8_t)vm_cond(cv, 0xf7, q);
                n = *nch;
                int16_t s = *S;
                *P = (int16_t)(*P + s);
                *A = s;
            } while (tries[n] < GP(uint8_t, DLLPTR(uint8_t, rv->cpc)[n])[1]);
        }
        n = *nch;
        DLLVAR(int16_t, rv->cpos0)[n] = *A;
        pc = GP(uint8_t, *PCv);
        if (tries[n] == 0)
            while (pc[0] != 0xf7) { pc += 2; SETPC(pc); }
        DLLVAR(int16_t, rv->cpos)[n] = *P;
        if (!bl) (*nch)--;
        *P = (int16_t)(*P - *S);
        return bl;
    }
    case 0xf4: {                            // repeated condition: at least pc[1], at most pc[2]
        uint8_t *start = pc;
        int16_t s0 = *S;
        int16_t si = (int16_t)(*P - s0);
        *last = si;
        int16_t q = *P;
        if (*A) while (TC(q) == 0x7e) q = (int16_t)(q + *A);
        *chg = TC(q);
        *P = q;
        int16_t n = start[2] == 0xdc ? 0x898 : start[2];
        *DLLVAR(int16_t, mv->maxcnt) = n;
        if (n > 0) {
            for (;;) {
                pc = start + 3;
                SETPC(pc);
                if (TC(q) == 0x0e) break;
                if (!(uint8_t)vm_cond(cv, 0xf4, q)) { pc = GP(uint8_t, *PCv); si = *last; break; }
                int16_t s = *S;
                si = *P;
                q = (int16_t)(si + s);
                *last = si;
                *P = q;
                while (TC(q) == 0x7e) { q = (int16_t)(q + s); *P = q; }
                cnt++;
                *chg = TC(q);
                if (!(cnt < *DLLVAR(int16_t, mv->maxcnt))) { pc = GP(uint8_t, *PCv); break; }
            }
        }
        *P = si;
        bl = cnt >= start[1];
        while (pc[0] != 0xf4) SETPC(++pc);
        break;
    }
    case 0xed: {                            // repeated test pc[3] (with pc[4]): at least pc[1], at most pc[2]
        int16_t s = *S;
        int16_t si = (int16_t)(*P - s);
        *last = si;
        int16_t q = *P;
        if (*A) while (TC(q) == 0x7e) q = (int16_t)(q + *A);
        *chg = TC(q);
        si = q;
        *P = si;
        int16_t n = pc[2] == 0xdc ? 0x898 : pc[2];
        *DLLVAR(int16_t, mv->maxcnt) = n;
        *DLLVAR(uint8_t, mv->clsb) = pc[4];
        uint8_t t = pc[3];
        if (t == 0xf2 || t == 0xf3 || (t >= 0xf9 && t <= 0xfc)) {
            for (; cnt < n; ) {
                uint8_t ch = TC(si);
                *chg = ch;
                if (ch == 0x0e) break;
                uint8_t a = *DLLVAR(uint8_t, mv->clsb);
                int pass;
                switch (t) {
                case 0xfc: pass = CLS(t2, a, ch) != 0; break;
                case 0xfb: pass = CLS(t2, a, ch) == 0; break;
                case 0xfa: pass = CLS(t1, a, ch) != 0; break;
                case 0xf9: pass = CLS(t1, a, ch) == 0; break;
                case 0xf3: pass = a == ch; break;
                default:   pass = a != ch; break;
                }
                if (!pass) break;
                *last = si;
                do { si = (int16_t)(si + s); *P = si; } while (TC(si) == 0x7e);
                cnt++;
            }
        }
        *P = *last;
        bl = cnt >= pc[1];
        SETPC(pc + 5);
        *A = s;
        return bl;
    }
    default: {                              // a literal (also 0xeb, 0xf8, 0xfe, 0xff)
        SKIPREAD();
        if (*chg != pc[0]) bl = 0;
        break;
    }
    }
    *A = *S;
    return bl;
#undef TC
#undef SETPC
#undef SKIPREAD
#undef CLS
}

uint8_t vm_run(int inst) {
    const RunVM *vm = &run_vm[inst];
    int16_t *step = DLLVAR(int16_t, vm->step), *nch = DLLVAR(int16_t, vm->nch);
    int16_t *pos0 = DLLVAR(int16_t, vm->pos0), *pos = DLLVAR(int16_t, vm->pos);
    GPTR(uint8_t) *pc = DLLPTR(uint8_t, vm->pc);
#define PCP GP(uint8_t, *pc)
    int16_t s = *step;
    GPSET(*pc, PCP + 1);
    *nch = 0;
    uint8_t ok = 1;
    *pos0 = s;
    *DLLVAR(uint8_t, vm->flag) = 1;
run:        // (the original has four copies of this loop; they behave the same)
    while (*PCP != 0xff) {
        if (!ok) goto backtrack;
        ok = vm_exec(inst);
        GPSET(*pc, PCP + 1);
        *pos = (int16_t)(*pos + *step);
    }
check:
    if (ok) return ok;
backtrack: {
        int16_t n = *nch;
        if (n <= 0) return ok;      // (ok is 0 here)
        uint8_t t = ++DLLVAR(uint8_t, vm->tries)[n];
        *pos = DLLVAR(int16_t, vm->cpos)[n];
        const uint8_t *at = GP(const uint8_t, DLLPTR(uint8_t, vm->cpc)[n]);
        *pos0 = DLLVAR(int16_t, vm->cpos0)[n];
        *pc = DLLPTR(uint8_t, vm->cpc)[n];
        *DLLPTR(char, vm->cur) = DLLPTR(char, vm->ctape)[n];
        switch (DLLVAR(int16_t, vm->ckind)[n]) {
        case 0xf5:
            (*nch)--;
            ok = 1;
            goto run;
        case 0xf6:
            if (!(uint8_t)vm_skip_block(&rule_vm[inst], 0xf6, t)) {
                GPSET(*pc, PCP + 2);
                (*nch)--;
                goto check;
            }
            GPSET(*pc, PCP + 2);
            ok = 1;
            goto run;
        case 0xf7:
            if (t > at[2]) {
                (*nch)--;
                goto check;
            }
            *DLLVAR(uint8_t, vm->flag) = 0;
            ok = 1;
            goto run;
        default:
            goto check;             // (the same choice point again: never happens in the rules)
        }
    }
#undef PCP
}

const MatchVM match_vm[3] = {
    { 0x63738cb8, 0x636a1d80, 0x63738cb0, 0x6373926c, 0x63738cac, 0x637391fc, 0x63738cb4 },
    { 0x637393ac, 0x636a5000, 0x637393a4, 0x63739440, 0x637393a0, 0x637393d4, 0x637393a8 },
    { 0x63739574, 0x636ab818, 0x6373956c, 0x63739620, 0x63739568, 0x637395b8, 0x63739570 },
};

// one class / literal test as the repetition opcode 0xed encodes it (the same tests as 0xf2..0xfc)
static int match_test(const CondVM *cv, uint8_t op, uint8_t arg, uint8_t ch) {
    switch (op) {
    case 0xf2: return ch != arg;
    case 0xf3: return ch == arg;
    case 0xf9: return DLLVAR(const uint8_t, cv->t1)[arg * 0xa2 + ch] == 0;
    case 0xfa: return DLLVAR(const uint8_t, cv->t1)[arg * 0xa2 + ch] != 0;
    case 0xfb: return DLLVAR(const uint8_t, cv->t2)[arg * 0xa2 + ch] == 0;
    case 0xfc: return DLLVAR(const uint8_t, cv->t2)[arg * 0xa2 + ch] != 0;
    }
    return 0;
}

int16_t vm_match(int inst, int32_t at, int32_t back) {
    const CondVM *cv = &cond_vm[inst];
    const MatchVM *mv = &match_vm[inst];
    GPTR(uint8_t) *PCv = DLLPTR(uint8_t, cv->pc);
    int16_t *adv = DLLVAR(int16_t, mv->adv);
    uint8_t *chg = DLLVAR(uint8_t, cv->ch);
    int16_t *last = DLLVAR(int16_t, mv->last);
    int16_t pos = (int16_t)at;
    uint32_t cnt = 0;                       // one counter for the whole call, as in the original
    uint8_t *pc = GP(uint8_t, *PCv);
#define TP() GP(char, *DLLPTR(char, cv->cur))
#define AT(i) ((uint8_t)TP()[(int16_t)(i)])
#define SKIP(step) do { if (*adv) while (AT(pos) == 0x7e) pos = (int16_t)(pos + (step)); } while (0)
#define READ() (*chg = AT(pos))
#define FAIL return 0
    for (;;) {
        uint8_t op = pc[0];
        switch (op) {
        case 0xff:
            return pos;
        case 0xfd: {                        // variable: bind (checking its class) or compare
            SKIP(1);
            uint8_t ch = READ(), v = pc[1];
            uint8_t b = DLLVAR(uint8_t, cv->vars)[v];
            if (b == 0) {
                const uint8_t *cl = DLLVAR(const uint8_t, cv->cls) + v * 2;
                if (cl[0] == 4) {
                    if (!DLLVAR(const uint8_t, cv->t4)[cl[1] * 0xa2 + ch]) FAIL;
                } else if (cl[0] == 5) {
                    if (!DLLVAR(const uint8_t, cv->t3)[cl[1] * 0xa2 + ch]) FAIL;
                }
                *DLLVAR(int16_t, cv->bound) = 1;
                DLLVAR(uint8_t, cv->vars)[v] = ch;
            } else if (ch != b) FAIL;
            goto next2;
        }
        case 0xea: {                        // not the (bound) variable
            SKIP(1);
            uint8_t ch = READ();
            uint8_t b = DLLVAR(uint8_t, cv->vars)[pc[1]];
            if (b != 0 && ch == b) FAIL;
            goto next2;
        }
        case 0xf2: case 0xf3: case 0xf9: case 0xfa: case 0xfb: case 0xfc: {
            SKIP(1);
            uint8_t ch = READ();
            if (ch == 0x0e || !match_test(cv, op, pc[1], ch)) FAIL;
            goto next2;
        }
        case 0xf8: {                        // class member: also yields its value
            SKIP(*adv);
            pc++;
            GPSET(*PCv, pc);
            uint8_t ch = READ();
            uint32_t k = pc[0] * 0xa2u + ch;
            if (DLLVAR(const uint8_t, cv->t1)[k] != 1) FAIL;
            *DLLVAR(uint8_t, mv->out) = DLLVAR(const uint8_t, mv->valtab)[k];
            goto next1;
        }
        case 0xf7: {                        // embedded condition up to 0xf7
            SKIP(*adv);
            READ();
            pc++;
            GPSET(*PCv, pc);
            if (AT(pos) == 0x0e) FAIL;
            if (!(uint8_t)vm_cond(cv, 0xf7, pos)) FAIL;
            pc = GP(uint8_t, *PCv);
            pos++;
            *adv = 1;
            pc++;
            GPSET(*PCv, pc);
            continue;
        }
        case 0xf4: {                        // repeated condition: min pc[1], max pc[2] (0xdc: 2200)
            *last = (int16_t)(pos - 1);
            SKIP(*adv);
            READ();
            uint8_t *start = pc;
            int16_t n = start[2] == 0xdc ? 0x898 : start[2];
            *DLLVAR(int16_t, mv->maxcnt) = n;
            cnt = 0;
            if (n > 0) {
                pc = start + 3;
                for (;;) {
                    GPSET(*PCv, pc);
                    if (AT(pos) == 0x0e) break;
                    if (!(uint8_t)vm_cond(cv, 0xf4, pos)) { pc = GP(uint8_t, *PCv); break; }
                    *last = pos;
                    pos++;
                    while (AT(pos) == 0x7e) pos++;
                    cnt++;
                    READ();
                    if (!((int16_t)cnt < *DLLVAR(int16_t, mv->maxcnt))) { pc = GP(uint8_t, *PCv); break; }
                }
            }
            if ((int16_t)cnt < (int16_t)start[1]) FAIL;
            while (*pc != 0xf4) { pc++; GPSET(*PCv, pc); }
            *adv = 1;
            pos = (int16_t)(*last + 1);
            goto next1;
        }
        case 0xed: {                        // repeated test: min pc[1], max pc[2], test pc[3] with pc[4]
            *last = (int16_t)(pos - 1);
            SKIP(1);
            READ();
            int16_t n = pc[2] == 0xdc ? 0x898 : pc[2];
            *DLLVAR(int16_t, mv->maxcnt) = n;
            uint8_t t = pc[3], arg = pc[4];
            *DLLVAR(uint8_t, mv->clsb) = arg;
            if (t == 0xf2 || t == 0xf3 || (t >= 0xf9 && t <= 0xfc)) {
                cnt = 0;
                if (n > 0) {
                    for (;;) {
                        uint8_t ch = READ();
                        if (ch == 0x0e || !match_test(cv, t, arg, ch)) break;
                        *last = pos;
                        do pos++; while (AT(pos) == 0x7e);
                        cnt++;
                        if (!((int16_t)cnt < n)) break;
                    }
                }
            }
            if ((int16_t)cnt < (int16_t)pc[1]) FAIL;
            *adv = 1;
            pc += 6;
            pos = (int16_t)(*last + 1);
            GPSET(*PCv, pc);
            continue;
        }
        case 0xef: case 0xf1:               // switch to tape pc[1]
            *DLLPTR(char, mv->saved) = *DLLPTR(char, cv->cur);
            *DLLPTR(char, cv->cur) = DLLPTR(char, cv->tapes)[pc[1]];
            *adv = 0;
            goto next2;
        case 0xee: case 0xf0:               // back to the previous tape
            *DLLPTR(char, cv->cur) = *DLLPTR(char, mv->saved);
            *adv = 0;
            goto next2;
        case 0xeb:                          // continue after `back`
            *adv = 1;
            pos = (int16_t)(back + 1);
            pc++;
            GPSET(*PCv, pc);
            continue;
        case 0xec:                          // skip filler
            while (AT(pos) == 0x7e) pos++;
            READ();
            *adv = 1;
            pc++;
            GPSET(*PCv, pc);
            continue;
        default: {                          // a literal character (also 0xf5, 0xf6, 0xfe)
            SKIP(1);
            if (READ() != op) FAIL;
            goto next1;
        }
        }
    next2:
        if (op != 0xef && op != 0xf1 && op != 0xee && op != 0xf0) { *adv = 1; pos++; }
        pc += 2;
        GPSET(*PCv, pc);
        continue;
    next1:
        if (op != 0xf4) { *adv = 1; pos++; }
        pc++;
        GPSET(*PCv, pc);
    }
#undef TP
#undef AT
#undef SKIP
#undef READ
#undef FAIL
}

LAYOUT(RuleHdr, tape, 0x294);
LAYOUT(RuleHdr, out, 0x298);
LAYOUT(RuleHdr, max, 0x290);

typedef struct ApplyVM {
    uint32_t wtape;         // the tape being written (char * global)
    uint32_t pos2;          // int32 global (RunVM.pos2): added to the segment start
    uint32_t tapes;         // the tape pointer array (as CondVM.tapes)
    uint32_t vars, out;     // variable bytes, the class value of the last 0xf8 match
    uint32_t maptab;        // class maps: [arg * row + out]
    uint32_t minlen;        // byte per rule set (a0): the least length of the replaced span
    int16_t kind;           // what 0xfe,2 writes to RuleHdr.kind
    int16_t row;            // class map row size (A 1, B and C 0x1a)
    int topcheck;           // B, C: fail (and abort) when a procedure grew the segment to the stack top
} ApplyVM;
static const ApplyVM apply_vm[3] = {
    { 0x63738cd4, 0x63739200, 0x63739204, 0x63738ce0, 0x63738cb0, 0x636a1ed2, 0x636a21a7, 0xb0, 1, 0 },
    { 0x637393bc, 0x637393d8, 0x637393dc, 0x637393c4, 0x637393a4, 0x636a51dd, 0x636a57bf, 0x15f, 0x1a, 1 },
    { 0x63739584, 0x637395bc, 0x637395bc, 0x63739590, 0x6373956c, 0x636abdbd, 0x636ad5df, 0x5bb, 0x1a, 1 },
};

uint8_t vm_apply(int inst, int32_t a0, RuleHdr *r) {
    const ApplyVM *av = &apply_vm[inst];
    const RuleVM *vm = &rule_vm[inst];
    GPTR(uint8_t) *PCv = DLLPTR(uint8_t, vm->pc);
    GPTR(char) *W = DLLPTR(char, av->wtape);
    int32_t *cnt = (int32_t *)(void *)((uint8_t *)r - 0x30);
#define CNT(i) cnt[(uint8_t)TAPE(vm, 0)[(int16_t)(i)]]
    uint8_t ok = 1;
    int32_t pad = 0, res;
    *W = r->out;
    int32_t p2 = *DLLVAR(int32_t, av->pos2);
    int16_t si = (int16_t)(r->left + (int16_t)p2);
    uint8_t *pc = GP(uint8_t, *PCv) + 1;
    GPSET(*PCv, pc);
    if (pc[0] == 0xfe) {
        pc++;
        GPSET(*PCv, pc);
        uint8_t n = pc[0];
        if (n == 1) { res = p2 - 1; goto done; }
        if (n == 2) { r->left = 0x500; r->kind = av->kind; res = p2 - 1; goto done; }
        for (int16_t i = (int16_t)(r->left - 1); i < (int32_t)si - 1; i++) CNT(i)--;
        vm_pop_range(vm, si, r->len);
        si--;
        r->len = si;
        ok = vm_callback(inst, n, &r->left, &r->len);
        if (av->topcheck && !(r->len < TOP(vm))) {
            *DLLVAR(int32_t, run_vm[inst].abort) = 1;
            r->applied = 0;
            return 0;
        }
        for (; si > r->len; si--) {
            char c5 = TAPE(vm, 5)[si - 1];
            if (c5) TAPE(vm, 5)[r->len - 1] = (char)(TAPE(vm, 5)[r->len - 1] + c5);
        }
        for (int16_t i = (int16_t)(r->left - 1); i < r->len; i++) CNT(i)++;
        res = ok ? (int16_t)(r->len - r->left) : 0;
        r->len = (int16_t)vm_push_until(vm, r->len);
        goto track;
    }
    {
        int32_t hi = (int16_t)DLLVAR(const uint8_t, av->minlen)[a0];
        if (pc[0] == 0xe9) {
            if ((int16_t)p2 > (int16_t)hi) { pad = p2 - hi; hi = p2; }
            else pad = 0;
            GPSET(*PCv, pc + 1);
        }
        int32_t d = hi - p2;
        res = hi - 1;
        for (int16_t i = (int16_t)(r->left - 1); i < (int32_t)si - 1; i++) CNT(i)--;
        if ((int16_t)d != 0) r->len = (int16_t)vm_move(vm, si, r->len, (int16_t)d);
        int16_t lim = (int16_t)(d + si - 2);
        for (int32_t i = (int16_t)(si - 1); i <= lim; i++)
            for (int k = 4; k >= 0; k--) TAPE(vm, k)[i] = 0x7e;
        int16_t w = r->left;
        uint8_t *p = GP(uint8_t, *PCv);
        for (; p[0] != 0xff; p = GP(uint8_t, *PCv) + 1, GPSET(*PCv, p)) {
            switch (p[0]) {
            case 0xec:
                for (int16_t m = (int16_t)pad; m >= 1; m--) GP(char, *W)[w++] = 0x7e;
                break;
            case 0xee: case 0xf0:               // (with an operand byte that is not used)
                GPSET(*PCv, p + 1);
                *W = r->out;
                break;
            case 0xef: case 0xf1:
                GPSET(*PCv, p + 1);
                *W = DLLPTR(char, av->tapes)[p[1]];
                break;
            case 0xf8:
                GPSET(*PCv, p + 1);
                GP(char, *W)[w++] = (char)DLLVAR(const uint8_t, av->maptab)[p[1] * av->row + *DLLVAR(uint8_t, av->out)];
                break;
            case 0xfd:
                GPSET(*PCv, p + 1);
                GP(char, *W)[w++] = (char)DLLVAR(uint8_t, av->vars)[p[1]];
                break;
            default:
                GP(char, *W)[w++] = (char)p[0];
            }
        }
        for (int16_t i = (int16_t)(r->left - 1); i < (int16_t)(w - 1); i++) CNT(i)++;
    }
done:
    ;
track:
    if (!r->track || (int16_t)res > r->max) r->max = (int16_t)res;
    return ok;
#undef CNT
}

uint32_t vm_try(int inst, int32_t a0, int32_t a1, RuleHdr *r, uint32_t prev_eax) {
    const RunVM *vm = &run_vm[inst];
    uint8_t ok = 0;
    if (inst == 0) {
        if (*DLLVAR(int16_t, rule_vm[0].top) - r->len < 2) {
            *DLLVAR(int32_t, vm->abort) = 1;
            r->applied = 0;
            return (prev_eax & 0xffff0000u) | ((uint16_t)r->len & 0xff00u);
        }
    } else if (*DLLVAR(int32_t, vm->abort) != 0) {
        return prev_eax & ~0xffu;
    }
    uint32_t eax = (prev_eax & 0xffff0000u) | (uint16_t)r->len;
    int16_t e = vm_match(inst, a1, (int32_t)eax);
    if (e != 0) {
        *DLLVAR(int16_t, vm->pos2) = (int16_t)(e - r->left);
        *DLLVAR(int16_t, vm->step) = -1;
        *DLLVAR(int16_t, vm->pos) = (int16_t)(r->left - 1);
        if (vm_run(inst)) {
            *DLLVAR(int16_t, vm->step) = 1;
            *DLLVAR(int16_t, vm->pos) = e;
            if (vm_run(inst) && vm_apply(inst, (int16_t)a0, r)) {
                r->applied = 1;
                ok = 1;
            }
        }
    }
    if (*DLLVAR(int16_t, vm->bound) != 0) {
        *DLLVAR(int16_t, vm->bound) = 0;
        uint8_t *v = DLLVAR(uint8_t, vm->vars);
        for (uint32_t i = 0; i < vm->nvars; i++) v[i] = 0;
    }
    *DLLPTR(char, vm->cur) = r->tape;
    return ((uint32_t)GRAW(r->tape) & ~0xffu) | ok;
}

typedef struct MainVM {
    uint32_t wtape, cur, out, vars, bound, abort;   // abort: 0 = instance A, which has no abort check
    uint32_t firsts, lasts;         // int16 per rule set: its first and last rule
    uint32_t offs;                  // int16 per rule: offset of its code
    uint32_t code;                  // the rule code: [offs] the rule type, then its bytecode
    uint32_t startoff, rlen;        // byte per rule: where scanning starts, the rule's input length
    uint32_t typ;                   // int16 global: the current rule type
    uint32_t ca, cb;                // byte per rule: characters that must occur in the segment
    uint32_t skip, cnt;             // byte per rule: rules to skip after it applied / its group size
    uint32_t tapes;
    uint32_t nvars;
} MainVM;
static const MainVM main_vm[3] = {
    { 0x63738cd4, 0x6373927c, 0x63738cb0, 0x63738ce0, 0x63721ccc, 0,
      0x636a07be, 0x636a07de, 0x636a1ee6, 0x636a2517, 0x636a2047, 0x636a20f7, 0x63738cd8,
      0x636a23b7, 0x636a2467, 0x636a2257, 0x636a2307, 0x63739204, 8 },
    { 0x637393bc, 0x63739448, 0x637393a4, 0x637393c4, 0x63721f2c, 0x63739564,
      0x636a32a6, 0x636a32c6, 0x636a523e, 0x636a5e9f, 0x636a54ff, 0x636a565f, 0x637393c0,
      0x636a5bdf, 0x636a5d3f, 0x636a591f, 0x636a5a7f, 0x637393dc, 7 },
    { 0x63739584, 0x63739628, 0x6373956c, 0x63739590, 0x63721f7c, 0x63739744,
      0x636a7c46, 0x636a7cc6, 0x636abee6, 0x636af29f, 0x636aca5f, 0x636ad01f, 0x63739588,
      0x636ae71f, 0x636aecdf, 0x636adb9f, 0x636ae15f, 0x637395bc, 30 },
};

uint32_t vm_main(int inst, int32_t mode, int32_t s, int16_t *pend) {
    const MainVM *m = &main_vm[inst];
    const RuleVM *vm = &rule_vm[inst];
    const RunVM *rv = &run_vm[inst];
    // the original's frame: ebp, 0x2a8 bytes of locals (the segment object at ebp-0x2a8), three saved
    // registers; the object's per-character counts reach 0x30 bytes below it
    uint8_t *frame = vc_stack_push(0x2b8, 0x30);
    RuleHdr *r = (RuleHdr *)(void *)(frame + 0xc);
    int32_t *counts = (int32_t *)(void *)((uint8_t *)r - 0x30);
    const uint8_t *ca = DLLVAR(const uint8_t, m->ca), *cb = DLLVAR(const uint8_t, m->cb);
    const uint8_t *skip = DLLVAR(const uint8_t, m->skip), *cnt = DLLVAR(const uint8_t, m->cnt);
    const int16_t *offs = DLLVAR(const int16_t, m->offs);
    const uint8_t *codebase = DLLVAR(const uint8_t, m->code);
    GPTR(uint8_t) *PCv = DLLPTR(uint8_t, vm->pc);
    int16_t *A = DLLVAR(int16_t, rv->pos0);
    GPTR(char) *cur = DLLPTR(char, m->cur);
#define CUR(i) ((uint8_t)GP(char, *cur)[(i)])
#define SETPC(p) GPSET(*PCv, (uint8_t *)(p))
#define TRY(k, at) ((uint8_t)vm_try(inst, (int16_t)(k), (int16_t)(at), r, 0))
#define SUBPC(j) SETPC(codebase + 1 + offs[(int16_t)(j)])
    // the original passes the length as a dword, with the two bytes after it in its upper half
#define LEN_DWORD(r) ((uint16_t)(r)->len | (uint32_t)((r)->_6[0] | (r)->_6[1] << 8) << 16)
    int32_t s1 = s + 1;
    int16_t old = *pend;
    *pend = (int16_t)(old + 1);
    vm_shift_right(vm, (int16_t)s, old);
    r->len = *pend;
    GPTR(char) t0 = *DLLPTR(char, vm->tape[0]);
    GPTR(char) t0m1;
    GPSET(t0m1, GP(char, t0) - 1);
    r->track = 0;
    *DLLPTR(char, m->wtape) = t0m1;
    *cur = t0m1;
    r->out = t0m1;
    r->tape = t0m1;
    r->applied = 0;
    *DLLVAR(int16_t, m->bound) = 0;
    *DLLVAR(uint8_t, m->out) = 0;
    for (uint32_t i = 0; i < m->nvars; i++) DLLVAR(uint8_t, m->vars)[i] = 0;
    for (int c = 0x0e; c < 0x0e + 0xa2; c++) counts[c] = 0;
    for (int16_t i = (int16_t)s; i < r->len; i++) counts[(uint8_t)GP(char, t0)[i]]++;
    counts[0x0e] = 2;
    int32_t mi = m->abort ? (int16_t)mode : mode;
    r->kind = (int16_t)(DLLVAR(const int16_t, m->firsts)[mi] - 1);
    int16_t end = DLLVAR(const int16_t, m->lasts)[mi];
    while (r->kind < end) {
        if (m->abort && *DLLVAR(int32_t, m->abort) != 0) {
            *pend = (int16_t)(r->len - 1);
            uint32_t e = vm_shift_left(vm, (int16_t)s1, r->len, LEN_DWORD(r));
            vc_stack_pop(frame, 0x2b8, 0x30);
            return e & ~0xffu;
        }
        int16_t i = ++r->kind;
        const uint8_t *code = codebase + offs[i];
        int16_t typ = code[0];
        int16_t so = DLLVAR(const uint8_t, m->startoff)[i];
        int16_t di = DLLVAR(const uint8_t, m->rlen)[i];
        *DLLVAR(int16_t, m->typ) = typ;
        if (typ > 0x64) { typ = (int16_t)(typ - 0x64); *DLLVAR(int16_t, m->typ) = typ; }
        if (typ > 0x63 || typ < 1 || (typ > 16 && typ != 0x63)) continue;
        if (typ == 0x63) {                  // select the tapes the following rules read and write
            r->tape = DLLPTR(char, m->tapes)[code[1]];
            *cur = r->tape;
            r->out = DLLPTR(char, m->tapes)[code[2]];
            *DLLPTR(char, m->wtape) = r->out;
            continue;
        }
        int16_t lo16 = (int16_t)(so + s1);                  // where a forward scan starts
        int32_t lo32 = (int32_t)so + (int16_t)s1;           // where a backward scan stops
        if (typ <= 8 && (!counts[ca[i]] || !counts[cb[i]])) continue;
        uint8_t ch = code[1];
        switch (typ) {
        case 1: case 3:                     // forward, anchored on a character; 1: every match, 3: the first
            r->left = lo16;
            for (int32_t p = r->left; p <= r->len - di; p = r->left) {
                if (CUR(p) == ch) {
                    if (typ == 1) r->max = 0;
                    SETPC(code + 2);
                    *A = 1;
                    uint8_t ok = TRY(r->kind, r->left + 1);
                    if (typ == 3) { if (ok) goto applied; r->left++; }
                    else r->left = (int16_t)(r->left + r->max + 1);
                } else r->left++;
            }
            continue;
        case 2: case 4: {                   // backward, anchored
            int16_t p = (int16_t)(r->len - di + 1);
            r->left = p;
            for (; p >= lo32; r->left = --p) {
                if (CUR(p) != ch) continue;
                *A = 1;
                SETPC(code + 2);
                uint8_t ok = TRY(r->kind, r->left + 1);
                if (typ == 4 && ok) goto applied;
                p = r->left;
            }
            continue;
        }
        case 5: case 7:                     // forward at every position; 5: every match, 7: the first
            r->left = lo16;
            if (r->left > r->len - di) continue;
            do {
                if (typ == 5) r->max = 0;
                *A = 0;
                SETPC(code + 1);
                uint8_t ok = TRY(r->kind, r->left);
                if (typ == 7) { if (ok) goto applied; r->left++; }
                else r->left = (int16_t)(r->left + r->max + 1);
            } while (r->left <= r->len - di);
            continue;
        case 6: case 8:                     // backward at every position
            r->left = (int16_t)(r->len - di + 1);
            if (r->left < lo32) continue;
            do {
                *A = 0;
                SETPC(code + 1);
                uint8_t ok = TRY(r->kind, r->left);
                if (typ == 8 && ok) goto applied;
                r->left--;
            } while (r->left >= lo32);
            continue;
        case 9:                             // a group, forward: every rule at every position
            r->track = 1;
            r->left = lo16;
            while (r->left <= r->len - di) {
                r->max = -2;
                int16_t si = (int16_t)(cnt[r->kind] + r->kind);
                for (; r->kind <= si; r->kind++) {
                    if (!counts[ca[r->kind]]) continue;
                    SUBPC(r->kind);
                    *A = 0;
                    TRY(r->kind, r->left);
                }
                r->left = (int16_t)(r->left + (r->max == -2 ? 1 : r->max + 1));
            }
            r->track = 0;
            continue;
        case 10: {                          // a group, backward
            int16_t p = (int16_t)(r->len - di + 1);
            r->left = p;
            while (p >= lo32) {
                int16_t si = (int16_t)(cnt[r->kind] + r->kind);
                for (; r->kind <= si; r->kind++) {
                    if (!counts[ca[r->kind]]) continue;
                    SUBPC(r->kind);
                    *A = 0;
                    TRY(r->kind, r->left);
                    p = r->left;
                }
                r->left = --p;
            }
            continue;
        }
        case 11: case 12: {                 // a group: stop after the position where any rule applied
            uint8_t found = 0;
            int16_t si = (int16_t)(cnt[i] + r->kind);
            int16_t p;
            if (typ == 11) r->left = lo16;
            else { p = (int16_t)(r->len - di + 1); r->left = p; }
            for (;;) {
                if (typ == 11 ? !(r->left <= r->len - di) : !(p >= lo32)) break;
                if (found) goto applied;
                for (int16_t j = r->kind; j <= si; j++) {
                    if (!counts[ca[j]]) continue;
                    SUBPC(j);
                    *A = 0;
                    if (TRY(j, r->left)) found = 1;
                    if (typ == 12) p = r->left;
                }
                if (typ == 11) r->left++;
                else r->left = --p;
            }
            if (found) goto applied;
            goto group_done;
        }
        case 13: case 14: {                 // a group: at each position, the first rule that applies
            int16_t p;
            if (typ == 13) {
                r->track = 1;
                r->left = lo16;
                if (!(r->left <= r->len - di)) goto group13;
            } else {
                p = (int16_t)(r->len - di + 1);
                r->left = p;
                if (!(p >= lo32)) goto group_done;
            }
            for (;;) {
                uint8_t bl = 0;
                if (typ == 13) r->max = -2;
                int16_t k = 0;
                do {
                    if (bl) break;
                    if (counts[ca[(int16_t)(r->kind + k)]]) {
                        SUBPC(r->kind + k);
                        *A = 0;
                        if (TRY(r->kind + k, r->left)) bl++;
                        if (typ == 14) p = r->left;
                    }
                    k++;
                } while (k <= cnt[r->kind]);
                if (typ == 13) {
                    r->left = (int16_t)(r->left + (bl ? r->max + 1 : 1));
                    if (!(r->left <= r->len - di)) break;
                } else {
                    r->left = --p;
                    if (!(p >= lo32)) break;
                }
            }
            if (typ == 14) goto group_done;
        group13:
            r->kind = (int16_t)(r->kind + cnt[r->kind]);
            r->track = 0;
            continue;
        }
        case 15: case 16: {                 // a group: stop at the first position where a rule applied
            uint8_t bl = 0;
            int16_t p;
            if (typ == 15) {
                r->left = lo16;
                if (!(r->left <= r->len - di)) goto group_end;
            } else {
                p = (int16_t)(r->len - di + 1);
                r->left = p;
                if (!(p >= lo32)) goto group_end;
            }
            for (;;) {
                if (bl) goto applied;
                int16_t k = 0;
                do {
                    if (bl) break;
                    if (counts[ca[(int16_t)(r->kind + k)]]) {
                        SUBPC(r->kind + k);
                        *A = 0;
                        if (TRY(r->kind + k, r->left)) bl++;
                        if (typ == 16) p = r->left;
                    }
                    k++;
                } while (k <= cnt[r->kind]);
                if (typ == 15) {
                    r->left++;
                    if (!(r->left <= r->len - di)) break;
                } else {
                    r->left = --p;
                    if (!(p >= lo32)) break;
                }
            }
        group_end:
            if (bl) goto applied;
            goto group_done;
        }
        }
        continue;
    applied:
        r->kind = (int16_t)(r->kind + skip[r->kind]);
        continue;
    group_done:
        r->kind = (int16_t)(r->kind + cnt[r->kind]);
    }
    *pend = (int16_t)(r->len - 1);
    uint32_t e = vm_shift_left(vm, (int16_t)s1, r->len, LEN_DWORD(r));
    uint8_t res = r->applied;
    vc_stack_pop(frame, 0x2b8, 0x30);
    return (e & ~0xffu) | res;
#undef CUR
#undef SETPC
#undef TRY
#undef SUBPC
#undef LEN_DWORD
}
