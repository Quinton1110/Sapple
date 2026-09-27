// Tiny Win32/COM environment around the x86 interpreter: PE loader, import thunks, heap,
// cooperative threads, events, critical sections, a posted-message queue, file access mapped to a
// host directory, and the bits of MSVCRT / KERNEL32 / USER32 / OLE32 a 1990s SAPI 4 engine imports.
#pragma once
#include "x86.h"
#include <stdio.h>

// Diagnostics. iOS builds define CV_NO_DEBUG_ENV: no stderr output at all (a screen-reader voice must
// never log anything that could carry spoken text) and no environment-variable debug hooks.
#ifdef CV_NO_DEBUG_ENV
#define EMU_LOG(...) ((void)0)
#else
#define EMU_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

typedef struct Emu Emu;
typedef void (*EmuHostFn)(Emu *e, X86 *c);

typedef struct EmuModule {
    char name[64];          // e.g. "MSTTSSYN.DLL"
    char win_path[260];     // what GetModuleFileNameA reports
    uint32_t base, size, entry;
    uint32_t export_dir, export_size;
    uint32_t pref_base;     // the image's preferred base (backtraces print addresses relative to it)
} EmuModule;

// memory layout (guest addresses)
#define EMU_THUNK_BASE   0x00020000u   // host-call thunks, 4 bytes each
#define EMU_THUNK_MAX    8192
#define EMU_STATIC_BASE  0x00030000u   // permanent small allocations (tables, vtables, strings)
#define EMU_STATIC_END   0x00100000u
#define EMU_STACK_BASE   0x00100000u   // 1 MB per thread
#define EMU_STACK_SIZE   0x00100000u
#define EMU_MAX_THREADS  15
#define EMU_IMAGE_BASE   0x01000000u   // first DLL; later ones at +16 MB
#define EMU_HEAP_BASE    0x04000000u

Emu *emu_create(uint32_t mem_size);   // NULL on failure
void emu_destroy(Emu *e);
X86 *emu_cpu(Emu *e);
int emu_failed(Emu *e);
const char *emu_error(Emu *e);

// Windows path prefix (e.g. "C:\\SPEECH") -> host directory. Case-insensitive file lookup.
void emu_map_dir(Emu *e, const char *win_prefix, const char *host_dir);

EmuModule *emu_load_pe(Emu *e, const char *host_path, const char *win_path, uint32_t load_base);
// Same, with flags. Imports naming an already-loaded guest module (e.g. MSVCP50.dll) resolve to its
// exports unless the host API table has an entry for that exact name (host overrides win).
#define EMU_LOAD_NO_CODE_CACHE 1   // do not give this module the predecoded-instruction cache
EmuModule *emu_load_pe_ex(Emu *e, const char *host_path, const char *win_path, uint32_t load_base, int flags);
uint32_t emu_get_export(Emu *e, EmuModule *m, const char *name);
uint32_t emu_get_export_ord(Emu *e, EmuModule *m, uint32_t ordinal);
int emu_dll_attach(Emu *e, EmuModule *m);

// Call guest code on the current thread. Works for stdcall and cdecl (ESP restored). Returns EAX.
uint32_t emu_call(Emu *e, uint32_t fn, int nargs, const uint32_t *args);
// COM: args exclude `this`; method = vtable slot
uint32_t emu_com(Emu *e, uint32_t obj, int method, int nargs, const uint32_t *args);
// Run other guest threads and dispatch posted messages until nothing more can happen.
void emu_pump(Emu *e);
// Called by emu_pump when the guest is idle; return nonzero if it did something (pump loops again).
void emu_set_idle_hook(Emu *e, int (*hook)(Emu *e, void *ctx), void *ctx);
// From a host fn: make emu_pump return as soon as the current thread yields (cancellation).
void emu_abort_pump(Emu *e);
void emu_clear_abort(Emu *e);
int emu_aborted(Emu *e);
// Watchdog: fault once the CPU has executed this many more instructions (0 = no limit).
void emu_set_insn_budget(Emu *e, uint64_t insns);
// Opaque pointer for the embedding code (host objects find their owner through it).
void emu_set_user(Emu *e, void *user);
void *emu_user(Emu *e);

// host functions exposed to the guest (thunks). argbytes: bytes popped on return (stdcall), 0 = cdecl,
// -1 = the function sets EIP/ESP itself.
uint32_t emu_thunk(Emu *e, const char *name, EmuHostFn fn, int argbytes);
// COM vtable of host methods: nargs excludes `this` (stdcall pops 4*(nargs+1))
typedef struct { const char *name; EmuHostFn fn; int nargs; } EmuMethod;
uint32_t emu_vtable(Emu *e, const EmuMethod *methods, int n);
void emu_block(Emu *e);          // from a host fn: suspend this thread, re-execute the call later

// guest memory
uint32_t emu_malloc(Emu *e, uint32_t size);
void emu_free(Emu *e, uint32_t p);
uint32_t emu_realloc(Emu *e, uint32_t p, uint32_t size);
uint32_t emu_static(Emu *e, uint32_t size);       // permanent, zeroed, 16-byte aligned
uint32_t emu_strdup(Emu *e, const char *s);        // on the heap
const char *emu_str(Emu *e, uint32_t a);           // host view of a guest NUL-terminated string
const uint16_t *emu_wstr(Emu *e, uint32_t a, size_t *len);
void *emu_ptr(Emu *e, uint32_t a, uint32_t n);

// posted-message queue (USER32)
void emu_post(Emu *e, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
// Per-thread message queues (off by default): a window belongs to the thread that created it; messages
// posted to a window owned by a guest worker thread go to that thread's queue (GetMessageA / PeekMessageA),
// messages for windows of the main (host) thread keep going to the queue emu_pump dispatches. Engines
// that run their own message loops (L&H TruVoice) need this; msttssyn keeps the original behaviour.
// The same mode reuses the slots of exited threads once all EMU_MAX_THREADS are used (TruVoice starts a
// thread per utterance) and frees a closed thread handle once its thread has exited.
void emu_enable_thread_queues(Emu *e);

// stats
typedef struct { uint64_t host_calls, insns, threads_created, switches, messages; } EmuStats;
EmuStats emu_stats(Emu *e);
extern int emu_trace_api;   // env SAPI4_TRACE_API=1
