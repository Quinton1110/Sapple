#pragma once
#include "emu.h"
#include <limits.h>
#include <string.h>
#include <stdlib.h>

enum { T_RUNNABLE, T_BLOCKED, T_WAIT_RETURN, T_HOST, T_DEAD };
enum { STOP_NONE, STOP_RETURN, STOP_BLOCK, STOP_YIELD, STOP_EXIT };

typedef struct Msg { uint32_t hwnd, msg, wp, lp; } Msg;
// growable message queue (Windows allows 10,000 posted messages per queue; this stops at 65,536)
typedef struct MsgQ { Msg *m; int head, n, cap; } MsgQ;
int mq_push(MsgQ *q, Msg m);            // 0 = full / out of memory
int mq_take(MsgQ *q, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, Msg *out);   // GetMessage filters
void mq_free(MsgQ *q);

typedef struct Thread {
    int id;
    int state;
    X86State st;            // saved context while not current
    uint32_t gen;           // e->gen when it blocked
    uint64_t wake_at;       // virtual ms deadline, 0 = infinite
    int timed_out;
    uint32_t teb, stack_lo, stack_hi;
    uint32_t handle;
    uint32_t last_error;
    uint32_t rand_seed;
    uint32_t strtok_next;
    uint32_t exit_code;
    MsgQ mq;                // per-thread message queue (only used with emu_enable_thread_queues)
    int handle_closed;      // CloseHandle was called on its handle while it ran (thread-queue mode)
} Thread;

enum { H_FREE, H_EVENT, H_THREAD, H_FILE, H_FIND, H_KEY };
typedef struct Handle {
    int type;
    void *obj;                  // H_KEY: struct RegKey
    int manual, signaled;       // event
    Thread *thread;             // thread
    FILE *fp;                   // file
    char **names; int count, pos; // find
} Handle;
#define EMU_MAX_HANDLES 256

typedef struct Frame {
    Thread *thread;
    uint32_t esp_lo, esp_hi;    // acceptable ESP range when the return thunk is reached
} Frame;

typedef struct HostEntry { const char *name; EmuHostFn fn; int argbytes; uint64_t calls; int owned_name; } HostEntry;

typedef struct Window { uint32_t hwnd, wndproc, userdata, extra[16]; int alive; int owner; } Window;
typedef struct WndClass { char name[64]; uint32_t wndproc; uint32_t cbWndExtra; } WndClass;

typedef struct VDir { char win[260]; char host[1024]; } VDir;

// heap: size-class free lists, blocks carry an 8-byte header [capacity][magic]
#define HEAP_CLASSES 48
struct Emu {
    X86 cpu;
    uint8_t *mem;
    uint32_t mem_size;
    char error[600];
    int failed;

    HostEntry hosts[EMU_THUNK_MAX];
    int nhosts;
    uint32_t static_next;

    uint32_t heap_next;
    uint32_t heap_free[HEAP_CLASSES];
    uint32_t heap_large;          // singly linked list of large free blocks
    uint64_t heap_live;

    Thread threads[EMU_MAX_THREADS];
    int nthreads;
    Thread *cur, *main;
    uint32_t gen;
    uint64_t vtime;
    int idle_spins;

    Frame frames[64];
    int nframes;
    int stop_reason;
    int retry;

    Handle handles[EMU_MAX_HANDLES];

    EmuModule modules[4];
    int nmodules;

    uint32_t thunk_return, thunk_thread_exit;

    WndClass classes[16]; int nclasses;
    Window windows[32]; int nwindows;
    MsgQ gq;                    // messages for windows of the main (host) thread: emu_pump dispatches them

    VDir vdirs[8]; int nvdirs;

    int (*idle_hook)(Emu *, void *);
    void *idle_ctx;
    void *user;
    int abort;                  // emu_abort_pump: pump returns at the next yield
    uint64_t insn_limit;        // absolute icount at which the watchdog fires (UINT64_MAX = none)
    int run_depth;

    // OLE structured storage host objects (ole.c)
    struct OleObj *ole_objs[512];
    uint32_t ole_stg_vt, ole_stm_vt;

    // CRT data
    uint32_t pctype_var, ctype_table, mb_cur_max, adjust_fdiv;
    uint32_t onexit[64]; int nonexit;
    struct { uint32_t g; FILE *fp; } crt_files[16];   // fopen: guest FILE* -> host stream (read-only on iOS)

    int thread_queues;          // emu_enable_thread_queues
    int host_msg;               // a worker posted to a main-thread window: emu_pump dispatches before running on
    struct RegKey *reg_roots[4]; // in-memory registry (registry.c): HKCR, HKCU, HKLM, HKU
    uint32_t malloc_obj;        // CoGetMalloc's host IMalloc (ole32), created on first use
    uint32_t iob;               // MSVCRT _iob[20] (FILE, 32 bytes each), created on first use
    uint32_t crt_data[8];       // other MSVCRT data imports (crt.c crt_data_defs), created when first imported

    EmuStats stats;
};

#define E_ARG(i) x86_arg(c, (i))
#define E_RET(v) (c->s.r[EAX] = (uint32_t)(v))

// shared helpers
Thread *emu_new_thread(Emu *e, uint32_t start, uint32_t arg, uint32_t stack_hint);
uint32_t emu_new_handle(Emu *e, int type);
Handle *emu_handle(Emu *e, uint32_t h);
void emu_close_handle(Emu *e, uint32_t h);
int emu_host_path(Emu *e, const char *win_path, char *out, size_t outsz, int must_exist);
void emu_register_crt(Emu *e);      // per-Emu CRT data (ctype tables ...)
uint32_t emu_crt_data_import(Emu *e, const char *name);   // address of an MSVCRT data export, 0 = not data
void emu_register_ole(Emu *e);      // per-Emu IStorage/IStream vtables
void emu_register_crt_apis(void);   // process-wide import tables, called once
void emu_register_winapi_apis(void);
void emu_register_ole_apis(void);
void emu_ole_cleanup(Emu *e);
void emu_register_registry_apis(void);
void emu_registry_cleanup(Emu *e);
// the calling guest thread's message queue helpers (winapi.c)
int emu_thread_post(Emu *e, Thread *t, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
Thread *emu_window_owner(Emu *e, uint32_t hwnd);
EmuHostFn emu_lookup_import(Emu *e, const char *dll, const char *name, int *argbytes, uint32_t *data_addr);
void emu_set_last_error(Emu *e, uint32_t err);

// CP1252
extern const uint16_t cp1252_to_uni[256];
uint8_t uni_to_cp1252(uint16_t u);
int cp1252_isalpha(uint8_t ch);
int cp1252_isupper(uint8_t ch);
int cp1252_islower(uint8_t ch);
uint8_t cp1252_toupper(uint8_t ch);
uint8_t cp1252_tolower(uint8_t ch);

// printf/scanf with guest varargs
int emu_format(Emu *e, char *out, size_t outsz, const char *fmt, uint32_t va, int wide_fmt);
int emu_scan(Emu *e, const char *str, const char *fmt, uint32_t va);

typedef struct { const char *dll, *name; EmuHostFn fn; int argbytes; } ApiDef;
void emu_add_apis(const ApiDef *defs, int n);
