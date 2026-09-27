#include "crt_vc.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void swap_elems(char *a, char *b, size_t w) {
    if (a == b) return;
    for (size_t i = 0; i < w; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}
static void shortsort(char *lo, char *hi, size_t w, int (*cmp)(const void *, const void *)) {
    while (hi > lo) {
        char *max = lo;
        for (char *p = lo + w; p <= hi; p += w)
            if (cmp(p, max) > 0) max = p;
        swap_elems(max, hi, w);
        hi -= w;
    }
}
void vc_qsort(void *base, size_t num, size_t w, int (*cmp)(const void *, const void *)) {
    if (num < 2 || w == 0) return;
    char *lostk[32], *histk[32];
    int sp = 0;
    char *lo = base, *hi = (char *)base + w * (num - 1);
recurse:;
    size_t size = (size_t)(hi - lo) / w + 1;
    if (size <= 8) {
        shortsort(lo, hi, w, cmp);
    } else {
        char *mid = lo + (size / 2) * w;
        swap_elems(mid, lo, w);
        char *loguy = lo, *higuy = hi + w;
        for (;;) {
            do { loguy += w; } while (loguy <= hi && cmp(loguy, lo) <= 0);
            do { higuy -= w; } while (higuy > lo && cmp(higuy, lo) >= 0);
            if (higuy < loguy) break;
            swap_elems(loguy, higuy, w);
        }
        swap_elems(lo, higuy, w);
        if (higuy - 1 - lo >= hi - loguy) {
            if (lo + w < higuy) { lostk[sp] = lo; histk[sp] = higuy - w; ++sp; }
            if (loguy < hi) { lo = loguy; goto recurse; }
        } else {
            if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
            if (lo + w < higuy) { hi = higuy - w; goto recurse; }
        }
    }
    --sp;
    if (sp >= 0) { lo = lostk[sp]; hi = histk[sp]; goto recurse; }
}

#ifndef DECOMP_HOOK
#include <stdlib.h>
void *vc_malloc(size_t n) { return malloc(n); }
void vc_free(void *p) { free(p); }
void *vc_realloc(void *p, size_t n) { return realloc(p, n); }
void *vc_calloc(size_t n, size_t size) { return calloc(n, size); }
void vc_sleep(uint32_t ms) { (void)ms; }
void *vc_stack_push(uint32_t size, uint32_t below) { char *p = malloc((size_t)size + below); return p ? p + below : NULL; }
void vc_stack_pop(void *frame, uint32_t size, uint32_t below) { (void)size; free((char *)frame - below); }
void vc_EnterCriticalSection(void *cs) { (void)cs; }
void vc_LeaveCriticalSection(void *cs) { (void)cs; }
// events of a single-threaded build: set / reset flags; a wait on nothing set times out (0x102)
#define VC_NEVENTS 4096
static struct { int used, manual, set; } vc_events[VC_NEVENTS];
uint32_t vc_CreateEventA(int32_t manual, int32_t initial) {
    for (uint32_t i = 1; i < VC_NEVENTS; i++)
        if (!vc_events[i].used) {
            vc_events[i].used = 1;
            vc_events[i].manual = manual != 0;
            vc_events[i].set = initial != 0;
            return 0x10000u + i;
        }
    return 0;
}
void vc_event_close(uint32_t h) {
    if (h > 0x10000u && h < 0x10000u + VC_NEVENTS) vc_events[h - 0x10000u].used = 0;
}
void vc_InitializeCriticalSection(void *cs) { (void)cs; }
static int vc_event(uint32_t h) { return h > 0x10000u && h < 0x10000u + VC_NEVENTS && vc_events[h - 0x10000u].used ? (int)(h - 0x10000u) : 0; }
int32_t vc_SetEvent(uint32_t h) { int i = vc_event(h); if (i) vc_events[i].set = 1; return i != 0; }
int32_t vc_ResetEvent(uint32_t h) { int i = vc_event(h); if (i) vc_events[i].set = 0; return i != 0; }
extern _Thread_local void (*vc_idle_hook)(void);
uint32_t vc_WaitForMultipleObjects(uint32_t n, const uint32_t *hs, int all, uint32_t ms) {
    (void)ms;
    if (all) {
        for (uint32_t k = 0; k < n; k++) { int i = vc_event(hs[k]); if (!i || !vc_events[i].set) return 0x102; }
        for (uint32_t k = 0; k < n; k++) { int i = vc_event(hs[k]); if (!vc_events[i].manual) vc_events[i].set = 0; }
        return 0;
    }
    for (uint32_t k = 0; k < n; k++) {
        int i = vc_event(hs[k]);
        if (i && vc_events[i].set) {
            if (!vc_events[i].manual) vc_events[i].set = 0;
            return k;
        }
    }
    if (ms == 0xffffffffu && vc_idle_hook) vc_idle_hook();
    return 0x102;       // (nothing else can set it: a wait would never end)
}
_Thread_local void (*vc_idle_hook)(void);
uint32_t vc_WaitForSingleObject(uint32_t h, uint32_t ms) { return vc_WaitForMultipleObjects(1, &h, 0, ms); }
int32_t vc_PostMessageA(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { (void)hwnd; (void)msg; (void)wp; (void)lp; return 1; }
int32_t vc_SetThreadPriority(uint32_t h, int32_t prio) { (void)h; (void)prio; return 1; }
void vc_DebugBreak(void) {}
uint32_t vc_GetCurrentThread(void) { return 0xfffffffeu; }
static char vc_module_path[1024] = "C:\\SPEECH\\msttssyn.dll", vc_user[260] = "User";
static uint32_t vc_last_error;
void vc_set_module_path(const char *path) { snprintf(vc_module_path, sizeof vc_module_path, "%s", path); }
void vc_set_user_name(const char *name) { snprintf(vc_user, sizeof vc_user, "%s", name); }
uint32_t vc_GetModuleFileNameA(uint32_t hmod, char *buf, uint32_t n) {
    (void)hmod;
    size_t l = strlen(vc_module_path);
    if (!n) return 0;
    if (l >= n) l = n - 1;
    memcpy(buf, vc_module_path, l);
    buf[l] = 0;
    return (uint32_t)l;
}
int32_t vc_GetUserNameA(char *buf, uint32_t *n) {
    size_t l = strlen(vc_user) + 1;
    if (*n < l) { *n = (uint32_t)l; vc_last_error = 122; return 0; }
    memcpy(buf, vc_user, l);
    *n = (uint32_t)l;
    return 1;
}
static FILE *vc_files_open[8];
uint32_t vc_CreateFileA(const char *path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t flags) {
    (void)access; (void)share; (void)disposition; (void)flags;
    char p[1024];
    size_t k = 0;
    for (; path[k] && k < sizeof p - 1; k++) p[k] = path[k] == '\\' ? '/' : path[k];
    p[k] = 0;
    int slot = 0;
    while (slot < 8 && vc_files_open[slot]) slot++;
    FILE *f = slot < 8 ? fopen(p, "rb") : NULL;
    if (!f) { vc_last_error = 2; return 0xffffffffu; }
    vc_files_open[slot] = f;
    return 0x200u + (uint32_t)slot;
}
static FILE *vc_file(uint32_t h) { return h >= 0x200u && h < 0x208u ? vc_files_open[h - 0x200u] : NULL; }
uint32_t vc_GetFileSize(uint32_t h) {
    FILE *f = vc_file(h);
    if (!f) { vc_last_error = 6; return 0xffffffffu; }
    long pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, pos, SEEK_SET);
    return (uint32_t)n;
}
int32_t vc_ReadFile(uint32_t h, void *buf, uint32_t n, uint32_t *got) {
    FILE *f = vc_file(h);
    if (!f) { vc_last_error = 6; return 0; }
    *got = (uint32_t)fread(buf, 1, n, f);
    return 1;
}
int32_t vc_CloseHandle(uint32_t h) {
    FILE *f = vc_file(h);
    if (!f) return 0;
    fclose(f);
    vc_files_open[h - 0x200u] = NULL;
    return 1;
}
uint32_t vc_GetLastError(void) { return vc_last_error; }
// storage objects of the portable build: a storage (a compound file's root) or a stream (its bytes)
#include "cfb.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
typedef struct VcObj { int kind; int refs; Cfb *f; uint32_t dir; uint8_t *data; size_t len, pos; } VcObj;
static void *vc_obj(int kind, Cfb *f, uint32_t dir) {
    VcObj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = kind;
    o->refs = 1;
    o->f = f;
    o->dir = dir;
    return o;
}
void *vc_stg_open_file(const char *path) {
    Cfb *f = cfb_open(path);
    return f ? vc_obj(1, f, 0) : NULL;
}
void *vc_stg_open_mem(const void *data, size_t len) {
    Cfb *f = cfb_open_mem(data, len);
    return f ? vc_obj(1, f, 0) : NULL;
}
int32_t vc_stg_open_stream(void *stg, const uint16_t *name, void **stm) {
    VcObj *s = stg;
    char n[64];
    int k = 0;
    for (; name[k] && k < 63; k++) n[k] = (char)name[k];
    n[k] = 0;
    *stm = NULL;
    int i = cfb_find(s->f, s->f->ents[s->dir].child, n);
    if (i < 0 || s->f->ents[i].type != 2) return (int32_t)0x80030002u;     // STG_E_FILENOTFOUND
    VcObj *o = vc_obj(2, s->f, (uint32_t)i);
    s->f->refs++;
    o->data = cfb_stream(s->f, &s->f->ents[i], &o->len);
    *stm = o;
    return 0;
}
int32_t vc_stm_read(void *stm, void *buf, uint32_t n, uint32_t *got) {
    VcObj *o = stm;
    size_t k = o->pos < o->len ? o->len - o->pos : 0;
    if (k > n) k = n;
    memcpy(buf, o->data + o->pos, k);
    o->pos += k;
    if (got) *got = (uint32_t)k;
    return 0;       // (S_OK even on a short read, as OLE's)
}
int32_t vc_stm_size(void *stm, uint32_t *size) {
    *size = (uint32_t)((VcObj *)stm)->len;
    return 0;
}
int32_t vc_stm_seek(void *stm, int32_t move, uint32_t origin, uint32_t *pos) {
    VcObj *o = stm;
    int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)o->pos : (int64_t)o->len;
    int64_t np = base + move;
    if (np < 0) return (int32_t)0x80030019u;       // STG_E_INVALIDFUNCTION
    o->pos = (size_t)np;
    if (pos) *pos = (uint32_t)np;
    return 0;
}
static const VcFile *vc_files;
static int vc_nfiles;
void vc_files_use(const VcFile *files, int n) { vc_files = files; vc_nfiles = n; }
int32_t vc_StgOpenStorage(const uint16_t *path, uint32_t mode, void **stg) {
    (void)mode;
    char p[1024];
    size_t k = 0;
    for (; path[k] && k < sizeof p - 1; k++) p[k] = path[k] == '\\' ? '/' : (char)path[k];
    p[k] = 0;
    if (vc_files) {
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        *stg = NULL;
        for (int i = 0; i < vc_nfiles; i++)
            if (!strcasecmp(vc_files[i].name, base)) *stg = vc_stg_open_mem(vc_files[i].data, vc_files[i].size);
        return *stg ? 0 : (int32_t)0x80030002u;
    }
    *stg = vc_stg_open_file(p);
    return *stg ? 0 : (int32_t)0x80030002u;
}
#include <dirent.h>
#include <strings.h>
typedef struct VcFind { char **names; int count, pos; } VcFind;
static VcFind vc_finds[8];
static int wild(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    if (!*s) return 0;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    return wild(p + 1, s + 1);
}
static void find_fill(void *data, const char *name) {
    memset(data, 0, 0x140);
    *(uint32_t *)data = 0x20;       // FILE_ATTRIBUTE_ARCHIVE
    snprintf((char *)data + 0x2c, 260, "%s", name);
}
uint32_t vc_FindFirstFileA(const char *pattern, void *data) {
    char dir[1024], pat[260];
    const char *bs = strrchr(pattern, '\\');
    if (bs) { snprintf(dir, sizeof dir, "%.*s", (int)(bs - pattern), pattern); snprintf(pat, sizeof pat, "%s", bs + 1); }
    else { snprintf(dir, sizeof dir, "."); snprintf(pat, sizeof pat, "%s", pattern); }
    for (char *q = dir; *q; q++) if (*q == '\\') *q = '/';
    int slot = 0;
    while (slot < 8 && vc_finds[slot].names) slot++;
    if (slot == 8) return 0xffffffffu;
    VcFind *f = &vc_finds[slot];
    if (vc_files) {
        for (int i = 0; i < vc_nfiles; i++) {
            if (!wild(pat, vc_files[i].name)) continue;
            f->names = realloc(f->names, sizeof(char *) * (size_t)(f->count + 1));
            f->names[f->count++] = strdup(vc_files[i].name);
        }
    } else {
        DIR *d = opendir(dir);
        if (!d) return 0xffffffffu;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || !wild(pat, de->d_name)) continue;
            f->names = realloc(f->names, sizeof(char *) * (size_t)(f->count + 1));
            f->names[f->count++] = strdup(de->d_name);
        }
        closedir(d);
    }
    for (int i = 1; i < f->count; i++)
        for (int j = i; j > 0 && strcasecmp(f->names[j - 1], f->names[j]) > 0; j--) {
            char *t = f->names[j]; f->names[j] = f->names[j - 1]; f->names[j - 1] = t;
        }
    if (!f->count) { free(f->names); f->names = NULL; return 0xffffffffu; }
    find_fill(data, f->names[0]);
    f->pos = 1;
    return 0x100u + (uint32_t)slot;
}
int32_t vc_FindNextFileA(uint32_t h, void *data) {
    if (h < 0x100u || h >= 0x108u || !vc_finds[h - 0x100u].names) return 0;
    VcFind *f = &vc_finds[h - 0x100u];
    if (f->pos >= f->count) return 0;
    find_fill(data, f->names[f->pos++]);
    return 1;
}
int32_t vc_FindClose(uint32_t h) {
    if (h < 0x100u || h >= 0x108u || !vc_finds[h - 0x100u].names) return 0;
    VcFind *f = &vc_finds[h - 0x100u];
    for (int i = 0; i < f->count; i++) free(f->names[i]);
    free(f->names);
    memset(f, 0, sizeof *f);
    return 1;
}
uint32_t vc_com_release(void *obj) {
    VcObj *o = obj;
    if (!o || --o->refs > 0) return o ? (uint32_t)o->refs : 0;
    if (o->kind == 2) free(o->data);
    cfb_release(o->f);
    free(o);
    return 0;
}
int32_t vc_CoInitialize(void) { return 0; }
void vc_CoUninitialize(void) {}
int32_t vc_CoCreateInstance(uint32_t clsid, uint32_t ctx, uint32_t iid, void **out) {
    (void)clsid; (void)ctx; (void)iid;
    *out = NULL;
    return (int32_t)0x80040154u;
}
int32_t vc_com_call(void *obj, int slot, int nargs, const uint32_t *args) {
    (void)obj; (void)slot; (void)nargs; (void)args;
    return (int32_t)0x80004005u;
}
static _Thread_local uint32_t vc_seed_own = 1;
static _Thread_local uint32_t *vc_seed;
void vc_rand_use(uint32_t *state) { vc_seed = state; }
int vc_rand(void) {
    uint32_t *s = vc_seed ? vc_seed : &vc_seed_own;
    *s = *s * 214013u + 2531011u;
    return (int)((*s >> 16) & 0x7fff);
}
#endif
