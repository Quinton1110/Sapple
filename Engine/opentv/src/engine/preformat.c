/*
 * Preformatter: characters from TextIn (or straight from the input ring when
 * TextIn is off) queue in pre_ring and are rewritten into mid_ring, the
 * stream the input stage turns into nodes.
 */
#include "engine.h"

/* Base letter for cp1252 0xC0..0xFF (e.g. "AAAAAOASEAEE..."). */
/* @0x1008c030 */
extern const uint8_t g_accent_base[64];
/* Second letter of a ligature or digraph (e.g. 'E' for Æ), indexed by the
 * full character code; zero when there is none. */
/* @0x1008bfb0 */
extern const uint8_t g_accent_second[256];

/* Fold a cp1252 character: returns the ASCII replacement for *c and stores
 * a second letter (or 0) back into *c.  0x80..0xBF fold to nothing. */
/* @0x1002c410 */
uint8_t TV_CDECL FoldAccent(uint8_t *c)
{
    uint8_t v = *c;
    if (v < 0x80) {
        *c = 0;
        return v;
    }
    if (v < 0xc0) {
        *c = 0;
        return 0;
    }
    *c = g_accent_second[v];
    return g_accent_base[v - 0xc0];
}

/* Queue one character for the preformatter and run it until the queue is
 * empty (unless already running further up the stack). */
/* @0x100050f0 */
void TV_THISCALL Preformat_PutChar(Engine *self, uint8_t c)
{
    int32_t n;
    if (!self->s2_1d55 && self->free_nodes > 0x260)
        self->s2_1d54 = 1;
    if (c == '\r' || c == '\n' || c == '\t')
        c = ' ';
    if (c == 0 || c == 0x7f)
        return;
    if (c < 0x20 && c != 0x1b)
        return;
    n = self->pre_rd - self->pre_wr - 1;
    if (n < 0)
        n += 0x100;
    if (n <= 0)
        return;
    self->pre_ring[self->pre_wr] = c;
    self->pre_wr = (self->pre_wr + 1) & 0xff;
    if (self->pre_busy)
        return;
    self->pre_busy = 1;
    while (self->pre_rd != self->pre_wr)
        Preformat_Run(self);
    self->pre_busy = 0;
}

/* Bit masks for ESC[..A / ESC[..N parameters: g_bit_for_param[n] = 1 << (n-1). */
/* @0x100f839c */
extern const uint32_t g_bit_for_param[17];
/* Largest rate index allowed for each rate class (ESC[..f). */
/* @0x100b5bd0 */
extern const int32_t g_rate_class_max[10];
/* Largest raw value of each synthesis parameter (ESC[k;v l). */
/* @0x100f83e0 */
extern const int32_t g_param_max[22];
/* @0x100f8530 */
extern const uint8_t g_default_params[22];

/* @0x100047c0 */
void TV_THISCALL Preformat_Reset(Engine *self)
{
    self->esc_state = 1;
    self->pre_busy = 0;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Take one character from pre_ring.  Plain text is folded to ASCII and
 * copied to mid_ring; "ESC [ p1 ; p2 ... X" commands (ANSI CSI syntax, up
 * to 16 numeric parameters) update the engine's voice settings and are
 * re-emitted into mid_ring in binary form: ESC, letter, n, n bytes. */
/* @0x10004840 */
void TV_THISCALL Preformat_Run(Engine *self)
{
    uint8_t c = self->pre_ring[self->pre_rd];
    int32_t *param = self->esc_param;
    int32_t cnt, p0, nargs, k;
    uint32_t mask, v;

    self->pre_rd = (self->pre_rd + 1) & 0xff;
    if (c == 0x1b) {
        self->esc_state = 2;
        return;
    }
    switch (self->esc_state) {
    case 1: {
        uint8_t second = c, base;
        if (self->skip_text)
            return;
        base = FoldAccent(&second);
        if (base == 0)
            return;
        Engine_MidPut(self, base);
        if (second != 0)
            Engine_MidPut(self, second);
        return;
    }
    case 2:
        if (c == '[') {
            self->esc_state = 3;
            self->esc_overflow = 0;
            self->esc_nparam = 16;
            do {
                self->esc_nparam--;
                param[self->esc_nparam] = -1;
            } while (self->esc_nparam > 0);
        } else {
            self->esc_state = 1;
        }
        return;
    case 3:
        break;
    default:
        return;
    }

    if (c >= '0' && c <= '9') {
        if (self->esc_nparam >= 16) {
            /* past the parameter array: the original touches whatever
             * engine field lies there */
            Engine_ZeroDwordIfMinus1(self, 0x1af8 + 4u * self->esc_nparam);
            return;
        }
        if (param[self->esc_nparam] == -1)
            param[self->esc_nparam] = 0;
        param[self->esc_nparam] = param[self->esc_nparam] * 10 + c - '0';
        if (param[self->esc_nparam] > 255)
            self->esc_overflow = 1;
        return;
    }
    if (c == ';') {
        self->esc_nparam++;
        if (self->esc_nparam < 16)
            param[self->esc_nparam] = 0;
        return;
    }

    /* final character: execute the command */
    nargs = -1;
    cnt = self->esc_nparam;
    if (cnt != 0 || param[0] != -1)
        cnt++;
    if (self->esc_overflow || cnt > 16)
        c = 0;
    p0 = param[0];
    switch (c) {
    case 'A':
    case 'D':
        mask = 0;
        for (k = 0; k < cnt; k++)
            if (param[k] >= 1 && param[k] <= 7)
                mask |= g_bit_for_param[param[k]];
        if (c == 'A')
            self->flags_A |= mask;
        else
            self->flags_A &= ~mask;
        v = (uint32_t)self->flags_A;
        nargs = 2;
        c = 'A';
        p0 = v & 0xff;
        param[1] = (v >> 8) & 0xff;
        break;
    case 'N':
    case 'F':
        mask = 0;
        for (k = 0; k < cnt; k++)
            if (param[k] >= 1 && param[k] <= 16)
                mask |= g_bit_for_param[param[k]];
        if (c == 'N')
            self->flags_N |= mask;
        else
            self->flags_N &= ~mask;
        v = (uint32_t)self->flags_N;
        nargs = 2;
        c = 'N';
        p0 = v & 0xff;
        param[1] = (v >> 8) & 0xff;
        break;
    case 'C':
        if (cnt != 0)
            break;
        self->synth_hold = 0;
        nargs = 0;
        break;
    case 'H':
        if (cnt != 0)
            break;
        self->synth_hold = 1;
        nargs = -2;
        break;
    case 'I':
        if (cnt > 1)
            break;
        if (p0 < 0)
            p0 = 0;
        else if (p0 > 1)
            break;
        nargs = 1;
        self->mode_I = p0;
        break;
    case 'P':
        if (cnt > 1)
            break;
        if (p0 < 0)
            p0 = 1;
        else if (p0 > 1)
            break;
        nargs = 1;
        self->mode_P = p0;
        break;
    case 'S':
        if (cnt != 0)
            break;
        self->reset_pending = 1;
        nargs = -2;
        break;
    case 'V':
        if (cnt > 1)
            break;
        p0 = clamp(p0, 0, 9);
        nargs = 1;
        self->voice = p0;
        break;
    case 'a':
        if (cnt > 1)
            break;
        p0 = clamp(p0, 0, 16);
        nargs = 1;
        self->volume_atten = p0;
        break;
    case 'c':
        if (cnt > 1)
            break;
        if (p0 < 0 || p0 > 2)
            p0 = 0;
        nargs = 1;
        break;
    case 'f':
        if (cnt > 1)
            break;
        p0 = clamp(p0, 0, 9);
        self->rate_class = p0;
        if (self->rate_index >= g_rate_class_max[p0])
            self->rate_index = g_rate_class_max[p0];
        nargs = 2;
        param[1] = self->rate_index;
        break;
    case 'g':
    case 's':
        if (cnt > 1)
            break;
        p0 = p0 < 0 ? 100 : clamp(p0, 1, 255);
        nargs = 1;
        break;
    case 'i':
        if (cnt > 4)
            break;
        if (p0 < 0) {
            nargs = 1;
            p0 = 1;
        } else {
            nargs = cnt;
        }
        break;
    case 'l':
        if (p0 == -1 || cnt > 2 || p0 >= 22)
            break;
        if (param[1] < 0)
            param[1] = g_default_params[p0];
        else if (g_param_max[p0] < param[1])
            param[1] = g_param_max[p0];
        nargs = 2;
        self->cfg_bytes[p0] = (uint8_t)param[1];
        break;
    case 'p':
        if (cnt > 1)
            break;
        p0 = p0 < 0 ? 42 : clamp(p0, 25, 200);
        nargs = 1;
        self->pitch = p0 * 2;
        break;
    case 'r':
    case 'v':
        if (c == 'r') {
            if (cnt > 1)
                break;
            if (p0 < 50 && p0 > 0)
                p0 = 50;
            self->speed_wpm = p0 > 0 ? p0 : 150;
            if (p0 > 0)
                p0 = (p0 - 46) >> 3;
        }
        if (cnt > 1)
            break;
        if (p0 < 0)
            p0 = 13;
        else if (p0 > 25)
            p0 = 25;
        if (self->rate_class != 0 && p0 > g_rate_class_max[self->rate_class])
            p0 = g_rate_class_max[self->rate_class];
        nargs = 1;
        self->rate_index = p0;
        if (c == 'v')
            self->speed_wpm = p0 * 8 + 50;
        break;
    case 't':
        if (cnt > 3)
            break;
        nargs = 3;
        if (p0 < 0)
            p0 = 0;
        else if (p0 >= 10)
            nargs = -1;
        param[1] = param[1] < 0 ? 100 : clamp(param[1], 1, 255);
        param[2] = param[2] < 0 ? 50 : clamp(param[2], 30, 243);
        break;
    case 'w':
        if (cnt != 0)
            break;
        self->mode_I = 0;
        self->mode_P = 1;
        self->rate_index = 13;
        self->speed_wpm = 150;
        self->pitch = 85;
        self->volume_atten = 0;
        self->rate_class = 0;
        self->flags_N = 0x17c0;
        self->flags_A = 0x41;
        self->voice = 0;
        for (k = 0; k < 5; k++) {
            StageCtx *s = &self->stage_ctx[k];
            s->p_1c = self->mode_I;
            s->p_20 = self->mode_P;
            s->rate_index = self->rate_index;
            s->pitch = self->pitch;
            s->volume_atten = self->volume_atten;
            s->p_30 = self->rate_class;
            s->p_34 = self->flags_N;
            s->p_38 = self->flags_A;
            s->voice = self->voice;
        }
        nargs = -2;
        break;
    case 'x':
        if (cnt != 0)
            break;
        self->stop_mark = 1;
        self->synth_hold = 0;
        nargs = 0;
        break;
    default:
        nargs = -1;
        break;
    }

    param[0] = p0;
    if (nargs != -2 && !self->skip_text && Engine_MidFree(self) >= nargs + 3) {
        Engine_MidPut(self, 0x1b);
        Engine_MidPut(self, c);
        Engine_MidPut(self, (uint8_t)nargs);
        for (k = 0; k < nargs; k++)
            Engine_MidPut(self, (uint8_t)param[k]);
    }
    self->esc_state = 1;
}
