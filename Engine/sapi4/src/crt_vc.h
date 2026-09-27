// The Microsoft C runtime (MSVCRT, VC 5 era) behaviour the engine depends on, reproduced exactly:
// where the algorithm itself shows in the output (qsort's order of equal elements, rand's sequence).
#pragma once
#include <stddef.h>
#include <stdint.h>
// qsort exactly as the VC CRT does it (median-less quicksort, shortsort below 9 elements): elements
// that compare equal end up where the original put them.
void vc_qsort(void *base, size_t num, size_t width, int (*cmp)(const void *, const void *));
// rand() as MSVCRT has it (per-thread state, seed 1): the effects' RoboSoft warble depends on the
// exact sequence. In the hook build this advances the calling guest thread's own state.
int vc_rand(void);
#ifndef DECOMP_HOOK
// portable build: whose rand state vc_rand advances (a single-threaded driver running the engine's
// threads one after another gives each its own, starting at 1 as MSVCRT's per-thread state does;
// NULL: the calling thread's own)
void vc_rand_use(uint32_t *state);
#endif
// malloc / free on the engine's heap (in the hook build the guest heap, so the original code can use
// what the decompiled code allocates, in the same order and at the same addresses)
void *vc_malloc(size_t n);
void vc_free(void *p);
void *vc_realloc(void *p, size_t n);
void *vc_calloc(size_t n, size_t size);
// A function's stack frame, for locals whose address the code passes to other functions: `size` bytes,
// and `below` more under them that the code may also touch. The hook build puts it on the guest stack
// right under the return address, where the original's frame was, and moves the guest ESP below it
// until vc_stack_pop (so that original code called meanwhile uses the stack as it did); a portable
// build allocates it.
void *vc_stack_push(uint32_t size, uint32_t below);
void vc_stack_pop(void *frame, uint32_t size, uint32_t below);
// KERNEL32 Sleep: the engine yields its worker threads with Sleep(0). Nothing to do in a single-threaded
// build; the hook build advances the emulator's virtual clock and yields when the hook returns.
void vc_sleep(uint32_t ms);
#ifdef DECOMP_HOOK
void decomp_warn(const char *msg);
#endif

// The Win32 synchronisation and messaging calls the engine's thread plumbing makes. Handles and window
// handles are opaque 32-bit values. The hook build performs exactly what the emulator's KERNEL32/USER32
// do (blocking ones park the calling guest thread: only from HOOKY hooks); a single-threaded portable
// build gets trivial versions.
void vc_EnterCriticalSection(void *cs);
void vc_LeaveCriticalSection(void *cs);
// CreateEventA(NULL, manual, initial, NULL) / InitializeCriticalSection; a portable build keeps events
// in a small table (and critical sections are nothing)
uint32_t vc_CreateEventA(int32_t manual, int32_t initial);
void vc_InitializeCriticalSection(void *cs);
int32_t vc_SetEvent(uint32_t h);
int32_t vc_ResetEvent(uint32_t h);
uint32_t vc_WaitForSingleObject(uint32_t h, uint32_t ms);
uint32_t vc_WaitForMultipleObjects(uint32_t n, const uint32_t *hs, int all, uint32_t ms);
#ifndef DECOMP_HOOK
// portable build: called when a thread would wait forever (nothing set, no timeout); a single-threaded
// driver runs a stage's thread loop until then and leaves it from here (longjmp)
extern _Thread_local void (*vc_idle_hook)(void);
// release an event (a portable build's engine teardown)
void vc_event_close(uint32_t h);
#endif
int32_t vc_PostMessageA(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
int32_t vc_SetThreadPriority(uint32_t h, int32_t prio);
void vc_DebugBreak(void);
// KERNEL32 / ADVAPI32 file and identity calls of the user lexicon reader. A portable build answers
// GetModuleFileNameA with the path set by vc_set_module_path, GetUserNameA with "User" (as the
// emulator does) unless vc_set_user_name changed it, and reads files from the disk ('\\' as '/').
uint32_t vc_GetModuleFileNameA(uint32_t hmod, char *buf, uint32_t n);
int32_t vc_GetUserNameA(char *buf, uint32_t *n);
uint32_t vc_CreateFileA(const char *path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t flags);
uint32_t vc_GetFileSize(uint32_t h);
int32_t vc_ReadFile(uint32_t h, void *buf, uint32_t n, uint32_t *got);
int32_t vc_CloseHandle(uint32_t h);
uint32_t vc_GetLastError(void);
#ifndef DECOMP_HOOK
void vc_set_module_path(const char *path);
void vc_set_user_name(const char *name);
#endif
// KERNEL32 GetCurrentThread (a pseudo handle)
uint32_t vc_GetCurrentThread(void);
// IUnknown::Release on a COM object handed over by the SAPI layer (hook build: the guest object)
uint32_t vc_com_release(void *obj);
// OLE32 CoCreateInstance with a CLSID and IID in the DLL's data (by preferred-base address); *out the
// object (0 on failure). A portable build has no COM classes: it always fails (REGDB_E_CLASSNOTREG).
// OLE structured storage as the engine uses it (read-only): IStorage::OpenStream (name wide, share mode
// 0x10), IStream::Read (got may be NULL), IStream::Stat's size; objects released with vc_com_release.
// The hook build calls the emulator's objects through their vtables; a portable build has its own
// over src/cfb.c (vc_stg_open_file / vc_stg_open_mem give the root storage of a voice file).
int32_t vc_stg_open_stream(void *stg, const uint16_t *name, void **stm);
int32_t vc_stm_read(void *stm, void *buf, uint32_t n, uint32_t *got);
int32_t vc_stm_size(void *stm, uint32_t *size);
void *vc_stg_open_file(const char *path);
void *vc_stg_open_mem(const void *data, size_t len);
// OLE32 StgOpenStorage(path, NULL, mode, NULL, 0, &stg): a compound file's root storage by its wide path
// (a portable build maps '\\' to '/'); IStream::Seek (a 32-bit move; *pos, when given, the new position)
int32_t vc_StgOpenStorage(const uint16_t *path, uint32_t mode, void **stg);
int32_t vc_stm_seek(void *stm, int32_t move, uint32_t origin, uint32_t *pos);
// KERNEL32 FindFirstFileA / FindNextFileA / FindClose; data: a WIN32_FIND_DATAA (0x140 bytes, the name at
// 0x2c). A portable build lists the directory with the names sorted case-insensitively, as the emulator
// (and NTFS) return them.
uint32_t vc_FindFirstFileA(const char *pattern, void *data);
#ifndef DECOMP_HOOK
// portable build: files compiled into the program (a voice-baked build). Once set, the engine's
// directory listings and storage opens use this table instead of the disk; a path's directory part is
// ignored, names compare case-insensitively.
typedef struct VcFile { const char *name; const uint8_t *data; uint32_t size; } VcFile;
void vc_files_use(const VcFile *files, int n);
#endif
int32_t vc_FindNextFileA(uint32_t h, void *data);
int32_t vc_FindClose(uint32_t h);
// OLE32 CoInitialize(NULL) / CoUninitialize (nothing to do in a portable build)
int32_t vc_CoInitialize(void);
void vc_CoUninitialize(void);
int32_t vc_CoCreateInstance(uint32_t clsid, uint32_t ctx, uint32_t iid, void **out);
// method `slot` of a COM object, with 32-bit arguments (pointers already as the object sees them)
int32_t vc_com_call(void *obj, int slot, int nargs, const uint32_t *args);
