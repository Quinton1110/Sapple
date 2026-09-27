/*
 * Stored references into the engine's constant data.
 *
 * The data tables are the original image's bytes, and where the original
 * held an address it held four bytes.  They stay four bytes here whatever a
 * pointer is on the host, because the engine reads past the end of a table
 * into the one after it -- Stage3_GlideTab asks g_gt0_info for element 11 of
 * a table of four, and the value it gets back is load-bearing -- and that
 * only lands in the same place if nothing in the image moves.  Widening them
 * to real pointers moves everything after each one.
 *
 * So a stored address is a tv_ref: an offset from tv_data, which is where
 * tools/gen_data.py puts the whole extracted image.  Relative rather than
 * absolute so the data can be loaded anywhere, which matters for a library
 * that is someone else's DLL.
 *
 * Reading one:
 *
 *     const LtsEntry *e = TV_REF(LtsEntry, g_lts_rules[c]);
 *     const tv_ref   *p = TV_REF(tv_ref, g_lts_prefix[c]);   // and again
 *
 * A zero reference is the original's NULL; TV_REF_OK says whether there is
 * anything there.
 */
#ifndef TV_REF_H
#define TV_REF_H

#include <stdint.h>

#include "tv_common.h"

typedef uint32_t tv_ref;

#if defined(TV_HOOK_BUILD)
/* Hooked into the running DLL, the stored values are the addresses the
 * loader already fixed up, and the build is 32-bit, so they are pointers. */
#define TV_REF_AT(r)     ((const uint8_t *)(uintptr_t)(r))
#define TV_REF_OF(p)     ((tv_ref)(uintptr_t)(const uint8_t *)(p))
#else
/* The extracted data.  Generated: see tools/gen_data.py.  Offset 0 is left
 * empty so that a stored zero keeps meaning what it meant: nothing. */
extern const uint8_t tv_data[];
#define TV_REF_AT(r)     ((const uint8_t *)tv_data + (r))
#define TV_REF_OF(p)     ((tv_ref)((const uint8_t *)(p) - tv_data))
#endif

#define TV_REF(type, r)  ((const type *)(const void *)TV_REF_AT(r))
#define TV_REF_OK(r)     ((r) != 0)

#endif
