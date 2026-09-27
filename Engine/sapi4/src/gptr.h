// Pointer fields of the engine's structs. In the hook build (DECOMP_HOOK, the decompiled code running
// inside the emulator next to the original) a struct in guest memory stores 32-bit guest addresses, so
// the field is a uint32_t and GP() turns it into a host pointer; in the portable build it is simply a
// pointer. The same source compiles both ways, and in the hook build the layouts are the original's.
#pragma once
#include <stdint.h>
#ifdef DECOMP_HOOK
extern _Thread_local uint8_t *decomp_guest_mem;
#define GPTR(T) uint32_t
#define GP(T, v) ((T *)(void *)(decomp_guest_mem + (v)))
// the same, NULL for a null field
#define GPN(T, v) ((v) ? GP(T, v) : (T *)0)
static inline uint32_t decomp_g(const void *p) { return p ? (uint32_t)((const uint8_t *)p - decomp_guest_mem) : 0u; }
#define GPSET(field, p) ((field) = decomp_g(p))   // (p evaluated once)
// a global of the original DLL, by its preferred-base address (hook build: the live copy in the image)
extern uint32_t decomp_load_delta;
#define DLLVAR(T, addr) ((T *)(void *)(decomp_guest_mem + (uint32_t)((uint32_t)(addr) + decomp_load_delta)))
// a pointer-holding global of the DLL (T *): a 32-bit slot in the hook build
#define DLLPTR(T, addr) DLLVAR(GPTR(T), addr)
#define LAYOUT(T, field, off) _Static_assert(__builtin_offsetof(T, field) == (off), #T "." #field)
// a code address stored in the DLL's data (a function pointer table), as its preferred-base address
#define CODEADDR(v) ((uint32_t)(v) - decomp_load_delta)
// a code address of the DLL as the program would store it (hook build: relocated guest address)
#define DLLCODE(pref) ((uint32_t)(pref) + decomp_load_delta)
#define GRAW(v) ((uint32_t)(v))   // a pointer field as the 32-bit value the original passes around
#define GHOST(p) decomp_g(p)      // a host pointer into guest memory, as a guest address
#else
#define GPTR(T) T *
#define GP(T, v) ((T *)(v))
#define GPN(T, v) ((T *)(v))
#define GPSET(field, p) ((field) = (p))
// portable build: the DLL's sections, loaded from the user's copy of the DLL at run time (port/dllimage.c);
// an address is the original's preferred-base address. Pointer-holding globals live in a separate
// table of host pointers (one slot per 32-bit global, so arrays of them index the same way), filled
// from the DLL's relocations.
void *decomp_dll_data(uint32_t addr);
void **decomp_dll_ptr(uint32_t addr);
#define DLLVAR(T, addr) ((T *)decomp_dll_data(addr))
#define DLLPTR(T, addr) ((T **)(void *)decomp_dll_ptr(addr))
#define CODEADDR(v) ((uint32_t)(uintptr_t)(v))   // extracted tables keep the original addresses
#define DLLCODE(pref) ((uint32_t)(pref))
#define GRAW(v) ((uint32_t)(uintptr_t)(v))
#define GHOST(p) ((uint32_t)(uintptr_t)(p))   // (only for calls a portable build never makes)
#define LAYOUT(T, field, off)
#endif
