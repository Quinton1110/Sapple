/*
 * Common definitions for the TruVoice decompilation.
 *
 * The same sources build two ways:
 *   TV_HOOK_BUILD  32-bit, linked into the harness; each decompiled function
 *                  replaces the original inside the running CGRM_EN.DLL, and
 *                  structs must match the original layout exactly.
 *   (default)      portable standalone build.
 *
 * Address annotations: a comment of the form  / * @0x1002c810 * /  directly
 * before a declaration names the function/global's address in CGRM_EN.DLL
 * (tools/gen_hookmap.py reads these).
 */
#ifndef TV_COMMON_H
#define TV_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(TV_HOOK_BUILD)
#  if !defined(__i386__)
#    error "the hook build must be compiled for 32-bit x86"
#  endif
/* Original calling conventions (MSVC 4.2): member functions pass `this` in
 * ECX and pop their own arguments. */
#  define TV_THISCALL __attribute__((thiscall))
#  define TV_STDCALL  __attribute__((stdcall))
#  define TV_CDECL    __attribute__((cdecl))
#  define TV_LAYOUT_ASSERT(x) _Static_assert(x, #x)
#else
/* Nothing outside the engine calls into it, so the standalone build can use
 * the compiler's own convention throughout. */
#  define TV_THISCALL
#  define TV_STDCALL
#  define TV_CDECL
/* The object still has the original's layout wherever pointers are four
 * bytes wide, and one place (layout.c) reaches into it by raw offset, so
 * keep checking it there too. */
#  if defined(__i386__)
#    define TV_LAYOUT_ASSERT(x) _Static_assert(x, #x)
#  else
#    define TV_LAYOUT_ASSERT(x) _Static_assert(1, "layout differs off 32-bit x86")
#  endif
#endif

#define TV_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#endif
