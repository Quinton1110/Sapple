#include "fe_small.h"
#include "crt_vc.h"

LAYOUT(TextSpan, f10, 0x10);
LAYOUT(FreeNode, next, 0x18);

int32_t span_f4(const TextSpan *s) { return s->f4; }
int32_t span_pos(const TextSpan *s) { return s->pos; }
int32_t span_len(const TextSpan *s) { return s->len; }
int32_t span_f10(const TextSpan *s) { return s->f10; }
char *span_cur(const TextSpan *s) { return GP(char, s->buf) + s->pos; }
char span_first(const TextSpan *s) { return GP(char, s->buf)[s->pos]; }
char span_last(const TextSpan *s) { return GP(char, s->buf)[s->pos + s->len - 1]; }
TextSpan *span_set_pos(TextSpan *s, int32_t v) { s->pos = v; return s; }
TextSpan *span_set_len(TextSpan *s, int32_t v) { s->len = v; return s; }
TextSpan *span_set_f10(TextSpan *s, int32_t v) { s->f10 = v; return s; }
TextSpan *span_clear(TextSpan *s) {
    s->buf = 0;
    s->f4 = s->pos = s->len = s->f10 = 0;
    return s;
}
TextSpan *span_pair_reset(TextSpan *s) {
    s->pos = s->len;
    return span_clear((TextSpan *)((char *)s + 0x10));   // (a second span overlapping this one at +0x10)
}
void pair_reset(Pair *p) { p->b = -1; p->a = 0; }
void block8_clear(Block8 *b) { for (int i = 0; i < 8; i++) b->v[i] = 0; }
int no_op_false8(void) { return 0; }
int no_op_zero8(void) { return 0; }
void free_cdecl(void *p) { vc_free(p); }
int free_if(void *p) {
    if (p) vc_free(p);
    return 0;
}
int is_digit_char(char c) { return (signed char)c >= '0' && (signed char)c <= '9'; }
uint16_t char_class16(uint16_t c) { return DLLVAR(const uint16_t, 0x636c8080)[c]; }
void freelist_push(FreeNode *n) {
    GPTR(FreeNode) *head = DLLPTR(FreeNode, 0x63738c9c);
    n->next = *head;
    GPSET(*head, n);
}
