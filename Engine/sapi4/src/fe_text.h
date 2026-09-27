// The front end's text rewriting before tokenising: the input is cut into records (escape sequences,
// then the text up to the next escape) and each text part may be rewritten: emoticons, lone symbols,
// e-mail and web addresses, four-digit numbers, plus signs, "~" as "approximately", and characters
// outside ASCII spelled out. Each record is 0xc9 bytes: the escapes at +0, the text at +0x65.
#pragma once
#include "gptr.h"

int txt_is_alpha(char c);                                   // @0x636822e4 stdcall: ASCII letter
int txt_approx(char c, char *out);                          // @0x636822ad stdcall
int txt_split(char *pre, char *s, char *post, char **rest);    // @0x6368231a stdcall
void txt_email_part(char *dst, const char *sep, const char *word);  // @0x63682b9e stdcall
int txt_emoticon(uint8_t *rec);                             // @0x63682540 stdcall
int txt_tilde(uint8_t *rec);                                // @0x636826b9 stdcall
int txt_normalize(uint8_t *rec);                            // @0x636827b0 stdcall
int txt_email(uint8_t *rec);                                // @0x63682de7 stdcall
int txt_thousands(uint8_t *rec);                            // @0x63682cd2 stdcall
int txt_plus(uint8_t *rec);                                 // @0x63683179 stdcall
int txt_tags(char **pp, uint8_t *recs, int32_t i);     // @0x63682437 stdcall
int txt_words(char **pp, uint8_t *recs, int32_t i);    // @0x636824df stdcall
int txt_rules(uint8_t *recs, int32_t i, int32_t spell);     // @0x636832d5 stdcall
// @0x6368337f stdcall: the rewritten text (malloc'd, into *out) if any record changed; returns 0
int32_t txt_rewrite(char *text, GPTR(char) *out, int32_t spell);
