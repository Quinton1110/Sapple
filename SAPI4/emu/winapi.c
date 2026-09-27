// KERNEL32 / USER32 / ADVAPI32 / OLE32 subset.
#include "emu_internal.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#define A(i) x86_arg(c, (i))
#define M (e->mem)
#define S(a) emu_str(e, (a))
#define PTR(a, n) ((char *)emu_ptr(e, (a), (n)))

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183
#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu

// ------------------------------------------------------------------ strings (lstr*, CompareString)
static void h_lstrcpyA(Emu *e, X86 *c) { const char *s = S(A(1)); size_t n = strlen(s) + 1; memmove(PTR(A(0), (uint32_t)n), s, n); E_RET(A(0)); }
static void h_lstrcpynA(Emu *e, X86 *c) {
    uint32_t n = A(2);
    if (!n) { E_RET(A(0)); return; }
    const char *s = S(A(1));
    size_t L = strlen(s);
    if (L > n - 1) L = n - 1;
    char *d = PTR(A(0), (uint32_t)L + 1);
    memmove(d, s, L);
    d[L] = 0;
    E_RET(A(0));
}
static void h_lstrlenA(Emu *e, X86 *c) { E_RET(A(0) ? strlen(S(A(0))) : 0); }

// Approximation of the Win32 "word sort" used by CompareStringA/lstrcmp(i)A: hyphen and apostrophe
// are ignored on the first pass; case differences only break ties (lowercase first).
static int word_weight_cmp(const uint8_t *a, int na, const uint8_t *b, int nb, int ignore_case) {
    int i = 0, j = 0;
    for (;;) {
        while (i < na && (a[i] == '-' || a[i] == '\'')) i++;
        while (j < nb && (b[j] == '-' || b[j] == '\'')) j++;
        if (i >= na || j >= nb) break;
        int x = cp1252_tolower(a[i]), y = cp1252_tolower(b[j]);
        if (x != y) return x < y ? -1 : 1;
        i++; j++;
    }
    if (i < na) return 1;
    if (j < nb) return -1;
    if (!ignore_case) {
        // tie-break on case: lowercase sorts before uppercase
        for (i = 0, j = 0; i < na && j < nb; i++, j++) {
            if (a[i] != b[j]) {
                int ua = cp1252_isupper(a[i]), ub = cp1252_isupper(b[j]);
                if (ua != ub) return ua ? 1 : -1;
            }
        }
    }
    // tie-break on the ignored punctuation
    int la = 0, lb = 0;
    for (i = 0; i < na; i++) la += a[i] == '-' || a[i] == '\'';
    for (j = 0; j < nb; j++) lb += b[j] == '-' || b[j] == '\'';
    if (la != lb) return la < lb ? -1 : 1;
    return 0;
}
static void h_lstrcmpA(Emu *e, X86 *c) {
    const char *a = S(A(0)), *b = S(A(1));
    E_RET((uint32_t)word_weight_cmp((const uint8_t *)a, (int)strlen(a), (const uint8_t *)b, (int)strlen(b), 0));
}
static void h_lstrcmpiA(Emu *e, X86 *c) {
    const char *a = S(A(0)), *b = S(A(1));
    E_RET((uint32_t)word_weight_cmp((const uint8_t *)a, (int)strlen(a), (const uint8_t *)b, (int)strlen(b), 1));
}
static void h_CompareStringA(Emu *e, X86 *c) {
    uint32_t flags = A(1);
    const char *a = S(A(2)), *b = S(A(4));
    int na = (int32_t)A(3), nb = (int32_t)A(5);
    if (na < 0) na = (int)strlen(a);
    if (nb < 0) nb = (int)strlen(b);
    int r = word_weight_cmp((const uint8_t *)a, na, (const uint8_t *)b, nb, (flags & 1) != 0);
    E_RET(r + 2);
}
static void h_MultiByteToWideChar(Emu *e, X86 *c) {
    uint32_t src = A(2), dst = A(4);
    int32_t n = (int32_t)A(3), dn = (int32_t)A(5);
    const uint8_t *s = (const uint8_t *)(n < 0 ? S(src) : PTR(src, (uint32_t)(n ? n : 1)));
    if (n < 0) n = (int32_t)strlen((const char *)s) + 1;
    if (dn == 0) { E_RET(n); return; }
    if (dn < n) { emu_set_last_error(e, 122); E_RET(0); return; }
    uint16_t *d = (uint16_t *)(void *)PTR(dst, 2u * (uint32_t)n);
    for (int i = 0; i < n; i++) d[i] = cp1252_to_uni[s[i]];
    E_RET(n);
}
static void h_WideCharToMultiByte(Emu *e, X86 *c) {
    uint32_t src = A(2), dst = A(4), useddef = A(7);
    int32_t n = (int32_t)A(3), dn = (int32_t)A(5);
    size_t wl;
    const uint16_t *s;
    if (n < 0) { s = emu_wstr(e, src, &wl); n = (int32_t)wl + 1; }
    else s = (const uint16_t *)(void *)PTR(src, 2u * (uint32_t)(n ? n : 1));
    if (dn == 0) { E_RET(n); return; }
    if (dn < n) { emu_set_last_error(e, 122); E_RET(0); return; }
    char *d = PTR(dst, (uint32_t)n);
    int used = 0;
    for (int i = 0; i < n; i++) { uint8_t ch = uni_to_cp1252(s[i]); if (ch == '?' && s[i] != '?') used = 1; d[i] = (char)ch; }
    if (useddef) wr32(c, useddef, (uint32_t)used);
    E_RET(n);
}

// ------------------------------------------------------------------ misc
static void h_GetLastError(Emu *e, X86 *c) { E_RET(e->cur->last_error); }
static void h_IsBadReadPtr(Emu *e, X86 *c) {
    uint32_t p = A(0), n = A(1);
    E_RET(n && (p < X86_LOW_LIMIT || (uint64_t)p + n > e->mem_size));
}
static void h_IsBadStringPtrA(Emu *e, X86 *c) {
    uint32_t p = A(0);
    if (p < X86_LOW_LIMIT || p >= e->mem_size) { E_RET(1); return; }
    E_RET(memchr(M + p, 0, e->mem_size - p) == NULL);
}
static void h_InterlockedIncrement(Emu *e, X86 *c) { (void)e; uint32_t v = rd32(c, A(0)) + 1; wr32(c, A(0), v); E_RET(v); }
static void h_InterlockedDecrement(Emu *e, X86 *c) { (void)e; uint32_t v = rd32(c, A(0)) - 1; wr32(c, A(0), v); E_RET(v); }
static void h_GetCurrentThread(Emu *e, X86 *c) { (void)e; E_RET(0xFFFFFFFEu); }
static void h_ret_true(Emu *e, X86 *c) { (void)e; E_RET(1); }
static void h_ret_zero(Emu *e, X86 *c) { (void)e; E_RET(0); }
static void h_DebugBreak(Emu *e, X86 *c) { (void)e; EMU_LOG("[guest] DebugBreak() ignored at %08x\n", rd32(c, c->s.r[ESP])); }
static void h_GetModuleFileNameA(Emu *e, X86 *c) {
    uint32_t h = A(0), buf = A(1), size = A(2);
    const char *path = "C:\\WINDOWS\\SAPI4HOST.EXE";
    for (int i = 0; i < e->nmodules; i++) if (e->modules[i].base == h) path = e->modules[i].win_path;
    size_t n = strlen(path);
    if (!size) { E_RET(0); return; }
    if (n >= size) n = size - 1;
    char *d = PTR(buf, (uint32_t)n + 1);
    memcpy(d, path, n);
    d[n] = 0;
    E_RET(n);
}
static void h_GetUserNameA(Emu *e, X86 *c) {
    const char *name = "User";
    uint32_t cnt = rd32(c, A(1));
    if (cnt < strlen(name) + 1) { wr32(c, A(1), (uint32_t)strlen(name) + 1); emu_set_last_error(e, 122); E_RET(0); return; }
    memcpy(PTR(A(0), (uint32_t)strlen(name) + 1), name, strlen(name) + 1);
    wr32(c, A(1), (uint32_t)strlen(name) + 1);
    E_RET(1);
}

// ------------------------------------------------------------------ critical sections
// CRITICAL_SECTION: +0 DebugInfo, +4 LockCount, +8 RecursionCount, +12 OwningThread, +16 LockSemaphore, +20 SpinCount
static void h_InitializeCriticalSection(Emu *e, X86 *c) { (void)e; memset(emu_ptr(e, A(0), 24), 0, 24); wr32(c, A(0) + 4, 0xFFFFFFFFu); }
static void h_DeleteCriticalSection(Emu *e, X86 *c) { (void)e; (void)c; }
static void h_EnterCriticalSection(Emu *e, X86 *c) {
    uint32_t cs = A(0);
    uint32_t owner = rd32(c, cs + 12);
    uint32_t me = (uint32_t)e->cur->id;
    if (owner == 0 || owner == me) {
        wr32(c, cs + 12, me);
        wr32(c, cs + 8, rd32(c, cs + 8) + 1);
        wr32(c, cs + 4, rd32(c, cs + 4) + 1);
        return;
    }
    e->cur->state = T_BLOCKED;
    e->cur->gen = e->gen;
    e->cur->wake_at = 0;
    emu_block(e);
}
static void h_LeaveCriticalSection(Emu *e, X86 *c) {
    uint32_t cs = A(0);
    uint32_t rc = rd32(c, cs + 8);
    if (rc == 0) { EMU_LOG("[emu] LeaveCriticalSection on unowned section %08x\n", cs); return; }
    wr32(c, cs + 8, rc - 1);
    wr32(c, cs + 4, rd32(c, cs + 4) - 1);
    if (rc == 1) { wr32(c, cs + 12, 0); e->gen++; }
}

// ------------------------------------------------------------------ events, waits, threads
static void h_CreateEventA(Emu *e, X86 *c) {
    uint32_t h = emu_new_handle(e, H_EVENT);
    Handle *p = emu_handle(e, h);
    p->manual = A(1) != 0;
    p->signaled = A(2) != 0;
    if (A(3)) EMU_LOG("[emu] CreateEventA named '%s' (names ignored)\n", S(A(3)));
    E_RET(h);
}
static void h_SetEvent(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (!p || p->type != H_EVENT) { E_RET(0); return; }
    p->signaled = 1;
    e->gen++;
    E_RET(1);
}
static void h_ResetEvent(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (!p || p->type != H_EVENT) { E_RET(0); return; }
    p->signaled = 0;
    E_RET(1);
}
static int is_signaled(Handle *p) {
    if (!p) return 0;
    if (p->type == H_EVENT) return p->signaled;
    if (p->type == H_THREAD) return p->thread ? p->thread->state == T_DEAD : p->signaled;
    return 0;
}
static void consume(Handle *p) { if (p->type == H_EVENT && !p->manual) p->signaled = 0; }

static void wait_block(Emu *e, X86 *c, uint32_t timeout) {
    (void)c;
    Thread *t = e->cur;
    t->state = T_BLOCKED;
    t->gen = e->gen;
    t->wake_at = timeout == INFINITE ? 0 : e->vtime + (timeout ? timeout : 1);
    emu_block(e);
}
static void h_WaitForSingleObject(Emu *e, X86 *c) {
    uint32_t h = A(0), timeout = A(1);
    Thread *t = e->cur;
    Handle *p = emu_handle(e, h);
    if (!p) { E_RET(WAIT_FAILED); return; }
    if (is_signaled(p)) { consume(p); t->timed_out = 0; E_RET(WAIT_OBJECT_0); return; }
    if (timeout == 0 || t->timed_out) { t->timed_out = 0; E_RET(WAIT_TIMEOUT); return; }
    wait_block(e, c, timeout);
}
static void h_WaitForMultipleObjects(Emu *e, X86 *c) {
    uint32_t n = A(0), arr = A(1), all = A(2), timeout = A(3);
    Thread *t = e->cur;
    if (n == 0 || n > 64) { E_RET(WAIT_FAILED); return; }
    if (all) {
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) if (!is_signaled(emu_handle(e, rd32(c, arr + 4 * i)))) ok = 0;
        if (ok) { for (uint32_t i = 0; i < n; i++) consume(emu_handle(e, rd32(c, arr + 4 * i))); t->timed_out = 0; E_RET(WAIT_OBJECT_0); return; }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            Handle *p = emu_handle(e, rd32(c, arr + 4 * i));
            if (!p) { E_RET(WAIT_FAILED); return; }
            if (is_signaled(p)) { consume(p); t->timed_out = 0; E_RET(WAIT_OBJECT_0 + i); return; }
        }
    }
    if (timeout == 0 || t->timed_out) { t->timed_out = 0; E_RET(WAIT_TIMEOUT); return; }
    wait_block(e, c, timeout);
}
static void h_Sleep(Emu *e, X86 *c) {
    e->vtime += A(0);
    // yield to other threads after returning
    x86_ret(c, 4);
    e->retry = 1;
    e->stop_reason = STOP_YIELD;
    c->stop = 1;
}
static void h_beginthreadex(Emu *e, X86 *c) {
    uint32_t start = A(2), arg = A(3), flags = A(4), tidp = A(5);
    if (flags & 4) x86_fault(c, "_beginthreadex CREATE_SUSPENDED not supported");
    Thread *t = emu_new_thread(e, start, arg, A(1));
    if (tidp) wr32(c, tidp, (uint32_t)t->id);
    if (emu_trace_api) EMU_LOG("[emu] thread T%d created at %08x arg %08x\n", t->id, start, arg);
    E_RET(t->handle);
}
static void h_CloseHandle(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (!p) { emu_set_last_error(e, ERROR_INVALID_HANDLE); E_RET(0); return; }
    emu_close_handle(e, A(0));
    E_RET(1);
}

// ------------------------------------------------------------------ files
static void h_CreateFileA(Emu *e, X86 *c) {
    const char *name = S(A(0));
    uint32_t access = A(1), disp = A(4);
    char host[1100];
    int write = (access & 0x40000000u) != 0;
#ifdef CV_NO_DEBUG_ENV
    // iOS builds: the guest may read its data files and nothing else - never create or write a file
    if (write) { emu_set_last_error(e, ERROR_ACCESS_DENIED); E_RET(INVALID_HANDLE_VALUE); return; }
#endif
    if (!emu_host_path(e, name, host, sizeof host, !write)) {
        if (emu_trace_api) EMU_LOG("[emu] CreateFileA('%s', %s) -> not found\n", name, write ? "write" : "read");
        emu_set_last_error(e, ERROR_FILE_NOT_FOUND);
        E_RET(INVALID_HANDLE_VALUE);
        return;
    }
    const char *mode = "rb";
    if (write) {
        struct stat st;
        int exists = stat(host, &st) == 0;
        if (disp == 1 && exists) { emu_set_last_error(e, 80); E_RET(INVALID_HANDLE_VALUE); return; } // CREATE_NEW
        if (disp == 3 && !exists) { emu_set_last_error(e, ERROR_FILE_NOT_FOUND); E_RET(INVALID_HANDLE_VALUE); return; }
        mode = (disp == 2 || disp == 1 || (disp == 4 && !exists) || disp == 5) ? "w+b" : "r+b";
    }
    FILE *f = fopen(host, mode);
    if (emu_trace_api) EMU_LOG("[emu] CreateFileA('%s', %s) -> %s%s\n", name, write ? "write" : "read", host, f ? "" : " (failed)");
    if (!f) { emu_set_last_error(e, errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND); E_RET(INVALID_HANDLE_VALUE); return; }
    uint32_t h = emu_new_handle(e, H_FILE);
    emu_handle(e, h)->fp = f;
    emu_set_last_error(e, 0);
    E_RET(h);
}
static void h_ReadFile(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    uint32_t n = A(2), readp = A(3);
    if (!p || p->type != H_FILE) { emu_set_last_error(e, ERROR_INVALID_HANDLE); E_RET(0); return; }
    size_t got = n ? fread(PTR(A(1), n), 1, n, p->fp) : 0;
    if (readp) wr32(c, readp, (uint32_t)got);
    E_RET(1);
}
static void h_WriteFile(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    uint32_t n = A(2), wp = A(3);
    if (!p || p->type != H_FILE) { emu_set_last_error(e, ERROR_INVALID_HANDLE); E_RET(0); return; }
#ifdef CV_NO_DEBUG_ENV
    (void)n;
    if (wp) wr32(c, wp, 0);
    emu_set_last_error(e, ERROR_ACCESS_DENIED);
    E_RET(0);
#else
    size_t put = n ? fwrite(PTR(A(1), n), 1, n, p->fp) : 0;
    if (wp) wr32(c, wp, (uint32_t)put);
    E_RET(put == n);
#endif
}
static void h_GetFileSize(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (!p || p->type != H_FILE) { E_RET(0xFFFFFFFFu); return; }
    long pos = ftell(p->fp);
    fseek(p->fp, 0, SEEK_END);
    long sz = ftell(p->fp);
    fseek(p->fp, pos, SEEK_SET);
    if (A(1)) wr32(c, A(1), 0);
    E_RET((uint32_t)sz);
}
static void h_CreateDirectoryA(Emu *e, X86 *c) {
#ifdef CV_NO_DEBUG_ENV
    emu_set_last_error(e, ERROR_ACCESS_DENIED);   // iOS builds: the guest never creates anything
    E_RET(0);
#else
    char host[1100];
    const char *name = S(A(0));
    if (!emu_host_path(e, name, host, sizeof host, 0)) { EMU_LOG("[emu] CreateDirectoryA('%s') refused\n", name); emu_set_last_error(e, ERROR_ACCESS_DENIED); E_RET(0); return; }
    struct stat st;
    if (stat(host, &st) == 0) { emu_set_last_error(e, ERROR_ALREADY_EXISTS); E_RET(0); return; }
    EMU_LOG("[emu] CreateDirectoryA('%s') -> %s\n", name, host);
    E_RET(mkdir(host, 0755) == 0);
#endif
}
static int wildmatch(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (wildmatch(pat + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return !strcmp(pat, ".") || !strcmp(pat, "*");
    if (*pat == '?' || tolower((uint8_t)*pat) == tolower((uint8_t)*s)) return wildmatch(pat + 1, s + 1);
    return 0;
}
static void fill_find(Emu *e, X86 *c, uint32_t fd, const char *dir, const char *name) {
    char full[1400];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    stat(full, &st);
    memset(emu_ptr(e, fd, 320), 0, 320);
    wr32(c, fd, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    wr32(c, fd + 32, (uint32_t)st.st_size);
    char *fn = PTR(fd + 44, 260);
    snprintf(fn, 260, "%s", name);
    char *alt = PTR(fd + 304, 14);
    snprintf(alt, 14, "%.12s", name);
}
static void h_FindFirstFileA(Emu *e, X86 *c) {
    const char *pat = S(A(0));
    char dirpart[520], filepat[260];
    const char *bs = strrchr(pat, '\\');
    if (bs) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(bs - pat), pat); snprintf(filepat, sizeof filepat, "%s", bs + 1); }
    else { snprintf(dirpart, sizeof dirpart, "."); snprintf(filepat, sizeof filepat, "%s", pat); }
    char host[1100];
    if (!emu_host_path(e, dirpart, host, sizeof host, 1)) {
        EMU_LOG("[emu] FindFirstFileA('%s') -> dir not mapped\n", pat);
        emu_set_last_error(e, ERROR_PATH_NOT_FOUND); E_RET(INVALID_HANDLE_VALUE); return;
    }
    DIR *d = opendir(host);
    if (!d) { emu_set_last_error(e, ERROR_PATH_NOT_FOUND); E_RET(INVALID_HANDLE_VALUE); return; }
    char **names = NULL; int count = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!wildmatch(filepat, de->d_name)) continue;
        names = realloc(names, sizeof(char *) * (size_t)(count + 1));
        names[count++] = strdup(de->d_name);
    }
    closedir(d);
    // deterministic order (NTFS returns names sorted case-insensitively)
    for (int i = 1; i < count; i++) for (int j = i; j > 0 && strcasecmp(names[j - 1], names[j]) > 0; j--) { char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t; }
    if (emu_trace_api) EMU_LOG("[emu] FindFirstFileA('%s') -> %d match(es)\n", pat, count);
    if (!count) { free(names); emu_set_last_error(e, ERROR_FILE_NOT_FOUND); E_RET(INVALID_HANDLE_VALUE); return; }
    uint32_t h = emu_new_handle(e, H_FIND);
    Handle *p = emu_handle(e, h);
    p->names = names; p->count = count; p->pos = 1;
    // remember host dir in names? store it as a trailing entry
    p->names = realloc(p->names, sizeof(char *) * (size_t)(count + 1));
    p->names[count] = strdup(host);
    fill_find(e, c, A(1), host, p->names[0]);
    E_RET(h);
}
static void h_FindNextFileA(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (!p || p->type != H_FIND) { E_RET(0); return; }
    if (p->pos >= p->count) { emu_set_last_error(e, ERROR_NO_MORE_FILES); E_RET(0); return; }
    fill_find(e, c, A(1), p->names[p->count], p->names[p->pos]);
    p->pos++;
    E_RET(1);
}
static void h_FindClose(Emu *e, X86 *c) {
    Handle *p = emu_handle(e, A(0));
    if (p && p->type == H_FIND) { free(p->names[p->count]); emu_close_handle(e, A(0)); }
    E_RET(1);
}

// ------------------------------------------------------------------ USER32
static void h_CharUpperBuffA(Emu *e, X86 *c) { uint32_t n = A(1); char *s = n ? PTR(A(0), n) : NULL; for (uint32_t i = 0; i < n; i++) s[i] = (char)cp1252_toupper((uint8_t)s[i]); E_RET(n); }
static void h_CharLowerBuffA(Emu *e, X86 *c) { uint32_t n = A(1); char *s = n ? PTR(A(0), n) : NULL; for (uint32_t i = 0; i < n; i++) s[i] = (char)cp1252_tolower((uint8_t)s[i]); E_RET(n); }
static void h_CharLowerA(Emu *e, X86 *c) {
    uint32_t p = A(0);
    if (p < 0x10000) { E_RET(cp1252_tolower((uint8_t)p)); return; }
    char *s = (char *)S(p);
    for (; *s; s++) *s = (char)cp1252_tolower((uint8_t)*s);
    E_RET(p);
}
static void h_IsCharAlphaA(Emu *e, X86 *c) { (void)e; E_RET(cp1252_isalpha((uint8_t)A(0))); }
static void h_IsCharAlphaNumericA(Emu *e, X86 *c) { (void)e; uint8_t ch = (uint8_t)A(0); E_RET(cp1252_isalpha(ch) || (ch >= '0' && ch <= '9') || ch == 0xB2 || ch == 0xB3 || ch == 0xB9); }
static void h_IsCharUpperA(Emu *e, X86 *c) { (void)e; E_RET(cp1252_isupper((uint8_t)A(0))); }
static void h_IsCharLowerA(Emu *e, X86 *c) { (void)e; E_RET(cp1252_islower((uint8_t)A(0))); }
static void h_wsprintfA(Emu *e, X86 *c) {
    char buf[1100];
    int n = emu_format(e, buf, sizeof buf, S(A(1)), c->s.r[ESP] + 12, 0);
    if (n > 1024) n = 1024;
    buf[n] = 0;
    memcpy(PTR(A(0), (uint32_t)n + 1), buf, (size_t)n + 1);
    E_RET(n);
}
static void h_RegisterClassA(Emu *e, X86 *c) {
    uint32_t wc = A(0);
    if (e->nclasses >= 16) { E_RET(0); return; }
    WndClass *k = &e->classes[e->nclasses++];
    k->wndproc = rd32(c, wc + 4);
    k->cbWndExtra = rd32(c, wc + 12);
    uint32_t nm = rd32(c, wc + 36);
    if (nm < 0x10000) snprintf(k->name, sizeof k->name, "#%u", nm);
    else snprintf(k->name, sizeof k->name, "%s", S(nm));
    if (emu_trace_api) EMU_LOG("[emu] RegisterClassA '%s' wndproc %08x\n", k->name, k->wndproc);
    E_RET(0xC000 + (uint32_t)e->nclasses);
}
static void h_CreateWindowExA(Emu *e, X86 *c) {
    uint32_t cls = A(1), param = A(11);
    char name[64];
    if (cls < 0x10000) snprintf(name, sizeof name, "#%u", cls); else snprintf(name, sizeof name, "%s", S(cls));
    WndClass *k = NULL;
    for (int i = 0; i < e->nclasses; i++) if (!strcasecmp(e->classes[i].name, name) || (cls >= 0xC000 && cls < 0x10000 && (uint32_t)(0xC001 + i) == cls)) k = &e->classes[i];
    if (!k) { EMU_LOG("[emu] CreateWindowExA: unknown class '%s'\n", name); E_RET(0); return; }
    if (e->nwindows >= 32) { E_RET(0); return; }
    Window *w = &e->windows[e->nwindows++];
    memset(w, 0, sizeof *w);
    w->hwnd = 0x00010000u + 0x10u * (uint32_t)e->nwindows;
    w->wndproc = k->wndproc;
    w->alive = 1;
    w->owner = e->cur ? e->cur->id : 1;
    uint32_t hwnd = w->hwnd;
    // CREATESTRUCTA for WM_NCCREATE / WM_CREATE
    uint32_t cs = emu_malloc(e, 48);
    memset(M + cs, 0, 48);
    wr32(c, cs + 0, param);
    wr32(c, cs + 4, A(10));
    wr32(c, cs + 36, A(2));
    wr32(c, cs + 40, cls);
    wr32(c, cs + 44, A(0));
    uint32_t a1[4] = { hwnd, 0x0081, 0, cs };
    uint32_t r = emu_call(e, w->wndproc, 4, a1);
    if (!r) { EMU_LOG("[emu] WM_NCCREATE returned 0\n"); }
    uint32_t a2[4] = { hwnd, 0x0001, 0, cs };
    r = emu_call(e, w->wndproc, 4, a2);
    emu_free(e, cs);
    if (emu_trace_api) EMU_LOG("[emu] CreateWindowExA class '%s' -> %08x (WM_CREATE %d)\n", name, hwnd, (int)r);
    if ((int32_t)r == -1) { w->alive = 0; E_RET(0); return; }
    E_RET(hwnd);
}
static Window *find_window(Emu *e, uint32_t hwnd) {
    for (int i = 0; i < e->nwindows; i++) if (e->windows[i].hwnd == hwnd && e->windows[i].alive) return &e->windows[i];
    return NULL;
}
static void h_DestroyWindow(Emu *e, X86 *c) {
    Window *w = find_window(e, A(0));
    if (!w) { E_RET(0); return; }
    uint32_t a[4] = { w->hwnd, 0x0002, 0, 0 };
    emu_call(e, w->wndproc, 4, a);
    uint32_t b[4] = { w->hwnd, 0x0082, 0, 0 };
    emu_call(e, w->wndproc, 4, b);
    w->alive = 0;
    E_RET(1);
}
static void h_IsWindow(Emu *e, X86 *c) { E_RET(find_window(e, A(0)) != NULL); }
static uint32_t *window_long(Emu *e, Window *w, int32_t idx) {
    if (idx == -21) return &w->userdata;
    if (idx == -4) return &w->wndproc;
    if (idx >= 0 && idx < 64 && !(idx & 3)) return &w->extra[idx / 4];
    (void)e;
    return NULL;
}
static void h_GetWindowLongA(Emu *e, X86 *c) {
    Window *w = find_window(e, A(0));
    uint32_t *p = w ? window_long(e, w, (int32_t)A(1)) : NULL;
    E_RET(p ? *p : 0);
}
static void h_SetWindowLongA(Emu *e, X86 *c) {
    Window *w = find_window(e, A(0));
    uint32_t *p = w ? window_long(e, w, (int32_t)A(1)) : NULL;
    if (!p) { E_RET(0); return; }
    uint32_t old = *p;
    *p = A(2);
    E_RET(old);
}
static void h_PostMessageA(Emu *e, X86 *c) {
    if (emu_trace_api) EMU_LOG("[T%d] PostMessage(%x, %x, %x, %x)\n", e->cur->id, A(0), A(1), A(2), A(3));
    if (e->thread_queues) {
        uint32_t hwnd = A(0);
        Thread *t = hwnd ? emu_window_owner(e, hwnd) : e->cur;
        if (!t) { emu_set_last_error(e, 1400); E_RET(0); return; }   // ERROR_INVALID_WINDOW_HANDLE
        if (t != e->main) { E_RET(emu_thread_post(e, t, hwnd, A(1), A(2), A(3))); return; }
        emu_post(e, A(0), A(1), A(2), A(3));
        E_RET(1);
        if (e->cur != e->main) {
            // a worker thread notifying the host thread (TruVoice delivers its audio this way): on Windows
            // the host thread would handle it right away, so yield and let emu_pump dispatch it now
            x86_ret(c, 16);
            e->retry = 1;
            e->stop_reason = STOP_YIELD;
            e->host_msg = 1;
            c->stop = 1;
        }
        return;
    }
    emu_post(e, A(0), A(1), A(2), A(3));
    E_RET(1);
}

// ------------------------------------------------------------------ per-thread message queues
Thread *emu_window_owner(Emu *e, uint32_t hwnd) {
    Window *w = find_window(e, hwnd);
    if (!w) return NULL;
    for (int i = 0; i < e->nthreads; i++) if (e->threads[i].id == w->owner) return e->threads[i].state == T_DEAD ? NULL : &e->threads[i];
    return NULL;
}
int emu_thread_post(Emu *e, Thread *t, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!mq_push(&t->mq, (Msg){ hwnd, msg, wp, lp })) { EMU_LOG("[emu] T%d message queue full, message %x dropped\n", t->id, msg); return 0; }
    e->gen++;                 // wakes a thread blocked in GetMessageA
    e->idle_spins = 0;
    return 1;
}
static void put_msg(Emu *e, X86 *c, uint32_t p, const Msg *m) {
    memset(emu_ptr(e, p, 28), 0, 28);
    wr32(c, p, m->hwnd); wr32(c, p + 4, m->msg); wr32(c, p + 8, m->wp); wr32(c, p + 12, m->lp);
    wr32(c, p + 16, (uint32_t)e->vtime);
}
static void h_GetMessageA(Emu *e, X86 *c) {
    Thread *t = e->cur;
    Msg m;
    if (!e->thread_queues || t == e->main) x86_fault(c, "GetMessageA on the host thread is not supported");
    if (mq_take(&t->mq, A(1), A(2), A(3), 1, &m)) {
        put_msg(e, c, A(0), &m);
        E_RET(m.msg == 0x12 ? 0 : 1);   // WM_QUIT
        return;
    }
    t->state = T_BLOCKED;
    t->gen = e->gen;
    t->wake_at = 0;
    emu_block(e);
}
static void h_PeekMessageA(Emu *e, X86 *c) {
    Thread *t = e->cur;
    Msg m;
    if (e->thread_queues && t != e->main && mq_take(&t->mq, A(1), A(2), A(3), A(4) & 1, &m)) {
        put_msg(e, c, A(0), &m);
        E_RET(1);
        x86_ret(c, 20);   // argbytes -1: this function pops its own arguments
        return;
    }
    E_RET(0);
    // nothing queued: let the other threads run before this one polls again
    x86_ret(c, 20);
    e->retry = 1;
    e->stop_reason = STOP_YIELD;
    c->stop = 1;
}
static void h_PostQuitMessage(Emu *e, X86 *c) {
    if (e->thread_queues && e->cur != e->main) emu_thread_post(e, e->cur, 0, 0x12, A(0), 0);
}
static void h_TranslateMessage(Emu *e, X86 *c) { (void)e; E_RET(0); }
static void h_DispatchMessageA(Emu *e, X86 *c) {
    uint32_t p = A(0);
    uint32_t hwnd = rd32(c, p);
    Window *w = hwnd ? find_window(e, hwnd) : NULL;
    if (!w) { E_RET(0); return; }
    uint32_t a[4] = { hwnd, rd32(c, p + 4), rd32(c, p + 8), rd32(c, p + 12) };
    E_RET(emu_call(e, w->wndproc, 4, a));
}
// Delivered synchronously on the calling thread (a cross-thread send runs the window procedure on the
// sender's thread; the engine only sends to its own windows and dialogs)
static void h_SendMessageA(Emu *e, X86 *c) {
    Window *w = find_window(e, A(0));
    if (!w) { E_RET(0); return; }
    if (emu_trace_api && e->thread_queues && w->owner != e->cur->id) EMU_LOG("[emu] cross-thread SendMessage %x to T%d from T%d\n", A(1), w->owner, e->cur->id);
    uint32_t a[4] = { A(0), A(1), A(2), A(3) };
    E_RET(emu_call(e, w->wndproc, 4, a));
}
static void h_CallWindowProcA(Emu *e, X86 *c) {
    uint32_t a[4] = { A(1), A(2), A(3), A(4) };
    E_RET(A(0) ? emu_call(e, A(0), 4, a) : 0);
}
static void h_ret_neg1(Emu *e, X86 *c) { (void)e; E_RET(0xFFFFFFFFu); }
static void h_GetRect(Emu *e, X86 *c) { if (A(1)) memset(emu_ptr(e, A(1), 16), 0, 16); E_RET(1); }

// LoadStringA from the module's RT_STRING resources (block uID/16 + 1, entry uID%16)
static uint32_t res_find(Emu *e, uint32_t base, uint32_t root, uint32_t dir, uint32_t id) {
    X86 *c = &e->cpu;
    uint32_t nnamed = rd16(c, dir + 12), nid = rd16(c, dir + 14);
    for (uint32_t i = 0; i < nnamed + nid; i++) {
        uint32_t ent = dir + 16 + 8 * i;
        if (rd32(c, ent) == id) return rd32(c, ent + 4);
    }
    (void)base; (void)root;
    return 0xFFFFFFFFu;
}
static void h_LoadStringA(Emu *e, X86 *c) {
    uint32_t base = A(0), id = A(1), buf = A(2);
    int32_t max = (int32_t)A(3);
    EmuModule *m = NULL;
    for (int i = 0; i < e->nmodules; i++) if (e->modules[i].base == base) m = &e->modules[i];
    if (!m || max <= 0) { E_RET(0); return; }
    uint32_t pe = base + rd32(c, base + 0x3C);
    uint32_t rsrc = rd32(c, pe + 24 + 96 + 16);
    if (!rsrc) { E_RET(0); return; }
    uint32_t root = base + rsrc;
    uint32_t r = res_find(e, base, root, root, 6);                          // RT_STRING
    if (r == 0xFFFFFFFFu || !(r & 0x80000000u)) { E_RET(0); return; }
    r = res_find(e, base, root, root + (r & 0x7FFFFFFFu), id / 16 + 1);
    if (r == 0xFFFFFFFFu || !(r & 0x80000000u)) { E_RET(0); return; }
    uint32_t lang = root + (r & 0x7FFFFFFFu);
    r = rd32(c, lang + 16 + 4);                                               // first language
    if (r & 0x80000000u) { E_RET(0); return; }
    uint32_t data = base + rd32(c, root + r), p = data;
    for (uint32_t k = 0; k < id % 16; k++) p += 2 + 2u * rd16(c, p);
    uint32_t n = rd16(c, p);
    if (n > (uint32_t)max - 1) n = (uint32_t)max - 1;
    char *d = PTR(buf, n + 1);
    for (uint32_t k = 0; k < n; k++) d[k] = (char)uni_to_cp1252(rd16(c, p + 2 + 2 * k));
    d[n] = 0;
    E_RET(n);
}
static void h_DefWindowProcA(Emu *e, X86 *c) { (void)e; E_RET(A(1) == 0x0081 ? 1 : 0); }
static void h_MessageBoxA(Emu *e, X86 *c) {
    EMU_LOG("[guest MessageBox] %s: %s\n", A(2) ? S(A(2)) : "", A(1) ? S(A(1)) : "");
    E_RET(1);
}
static void h_DialogBoxParamA(Emu *e, X86 *c) { (void)e; EMU_LOG("[emu] DialogBoxParamA ignored\n"); E_RET(0xFFFFFFFFu); (void)c; }
static void h_SetText(Emu *e, X86 *c) { (void)e; E_RET(1); }
static void h_LoadIconA(Emu *e, X86 *c) { (void)e; E_RET(0x7F00); }

// ------------------------------------------------------------------ more KERNEL32 (TruVoice)
static void h_GetWindowsDirectoryA(Emu *e, X86 *c) {
    const char *w = "C:\\WINDOWS";
    uint32_t n = (uint32_t)strlen(w), size = A(1);
    if (size <= n) { E_RET(n + 1); return; }
    memcpy(PTR(A(0), n + 1), w, n + 1);
    E_RET(n);
}
static void h_OutputDebugStringA(Emu *e, X86 *c) {
    if (emu_trace_api && A(0)) EMU_LOG("[guest debug] %s", S(A(0)));
}
static void h_GlobalAlloc(Emu *e, X86 *c) {
    uint32_t flags = A(0), n = A(1);
    if (flags & 2) x86_fault(c, "GlobalAlloc(GMEM_MOVEABLE) not supported");
    uint32_t p = emu_malloc(e, n ? n : 1);
    if (flags & 0x40) memset(PTR(p, n ? n : 1), 0, n ? n : 1);
    E_RET(p);
}
static void h_GlobalFree(Emu *e, X86 *c) { emu_free(e, A(0)); E_RET(0); }
static void h_LoadLibraryA(Emu *e, X86 *c) {
    // only comdlg32 / comctl32 for the engine's dialogs: not available here
    if (emu_trace_api) EMU_LOG("[emu] LoadLibraryA('%s') -> NULL\n", A(0) ? S(A(0)) : "");
    emu_set_last_error(e, 126);
    E_RET(0);
}
static void h_lstrcatA(Emu *e, X86 *c) {
    const char *s = S(A(1)); size_t n = strlen(s) + 1, d = strlen(S(A(0)));
    memmove(PTR(A(0) + (uint32_t)d, (uint32_t)n), s, n);
    E_RET(A(0));
}
static void h_InterlockedExchange(Emu *e, X86 *c) { (void)e; uint32_t v = rd32(c, A(0)); wr32(c, A(0), A(1)); E_RET(v); }
static void h_CreateThread(Emu *e, X86 *c) {
    uint32_t start = A(2), arg = A(3), flags = A(4), tidp = A(5);
    if (flags & 4) x86_fault(c, "CreateThread CREATE_SUSPENDED not supported");
    Thread *t = emu_new_thread(e, start, arg, A(1));
    if (tidp) wr32(c, tidp, (uint32_t)t->id);
    if (emu_trace_api) EMU_LOG("[emu] thread T%d created at %08x arg %08x (CreateThread)\n", t->id, start, arg);
    E_RET(t->handle);
}

// ------------------------------------------------------------------ OLE32
static void guid_str(Emu *e, uint32_t g, char *out) {
    X86 *c = &e->cpu;
    snprintf(out, 40, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(c, g), rd16(c, g + 4), rd16(c, g + 6),
             rd8(c, g + 8), rd8(c, g + 9), rd8(c, g + 10), rd8(c, g + 11), rd8(c, g + 12), rd8(c, g + 13), rd8(c, g + 14), rd8(c, g + 15));
}
static void h_CoCreateInstance(Emu *e, X86 *c) {
    char a[40], b[40];
    guid_str(e, A(0), a);
    guid_str(e, A(3), b);
    if (emu_trace_api) EMU_LOG("[emu] CoCreateInstance(clsid %s, iid %s) -> REGDB_E_CLASSNOTREG\n", a, b);
    if (A(4)) wr32(c, A(4), 0);
    E_RET(0x80040154u);
}

static void h_CoTaskMemAlloc(Emu *e, X86 *c) { E_RET(emu_malloc(e, A(0) ? A(0) : 1)); }
static void h_CoTaskMemFree(Emu *e, X86 *c) { emu_free(e, A(0)); }
static void h_CoTaskMemRealloc(Emu *e, X86 *c) { E_RET(emu_realloc(e, A(0), A(1))); }
static void h_StringFromCLSID(Emu *e, X86 *c) {
    char g[40];
    guid_str(e, A(0), g);
    uint32_t p = emu_malloc(e, 2 * 39);
    for (int i = 0; i < 39; i++) wr16(c, p + 2u * (uint32_t)i, (uint16_t)(uint8_t)g[i]);
    wr32(c, A(1), p);
    E_RET(0);
}
// IMalloc: QueryInterface, AddRef, Release, Alloc, Realloc, Free, GetSize, DidAlloc, HeapMinimize
static void im_QI(Emu *e, X86 *c) { wr32(c, A(2), A(0)); E_RET(0); (void)e; }
static void im_ref(Emu *e, X86 *c) { (void)e; E_RET(1); }
static void im_Alloc(Emu *e, X86 *c) { E_RET(emu_malloc(e, A(1) ? A(1) : 1)); }
static void im_Realloc(Emu *e, X86 *c) { E_RET(emu_realloc(e, A(1), A(2))); }
static void im_Free(Emu *e, X86 *c) { emu_free(e, A(1)); }
static void im_GetSize(Emu *e, X86 *c) { E_RET(A(1) ? rd32(c, A(1) - 8) : 0xFFFFFFFFu); (void)e; }
static void im_DidAlloc(Emu *e, X86 *c) { (void)e; E_RET(1); }
static void im_HeapMinimize(Emu *e, X86 *c) { (void)e; (void)c; }
static const EmuMethod imalloc_m[] = {
    { "IMalloc::QueryInterface", im_QI, 2 }, { "IMalloc::AddRef", im_ref, 0 }, { "IMalloc::Release", im_ref, 0 },
    { "IMalloc::Alloc", im_Alloc, 1 }, { "IMalloc::Realloc", im_Realloc, 2 }, { "IMalloc::Free", im_Free, 1 },
    { "IMalloc::GetSize", im_GetSize, 1 }, { "IMalloc::DidAlloc", im_DidAlloc, 1 }, { "IMalloc::HeapMinimize", im_HeapMinimize, 0 },
};
static void h_CoGetMalloc(Emu *e, X86 *c) {
    if (!e->malloc_obj) {
        e->malloc_obj = emu_static(e, 4);
        wr32(c, e->malloc_obj, emu_vtable(e, imalloc_m, 9));
    }
    wr32(c, A(1), e->malloc_obj);
    E_RET(0);
}

static const ApiDef win_apis[] = {
    { "KERNEL32.dll", "lstrcpyA", h_lstrcpyA, 8 },
    { "KERNEL32.dll", "lstrcpynA", h_lstrcpynA, 12 },
    { "KERNEL32.dll", "lstrlenA", h_lstrlenA, 4 },
    { "KERNEL32.dll", "lstrcmpA", h_lstrcmpA, 8 },
    { "KERNEL32.dll", "lstrcmpiA", h_lstrcmpiA, 8 },
    { "KERNEL32.dll", "CompareStringA", h_CompareStringA, 24 },
    { "KERNEL32.dll", "MultiByteToWideChar", h_MultiByteToWideChar, 24 },
    { "KERNEL32.dll", "WideCharToMultiByte", h_WideCharToMultiByte, 32 },
    { "KERNEL32.dll", "GetLastError", h_GetLastError, 0 },
    { "KERNEL32.dll", "IsBadReadPtr", h_IsBadReadPtr, 8 },
    { "KERNEL32.dll", "IsBadWritePtr", h_IsBadReadPtr, 8 },
    { "KERNEL32.dll", "IsBadStringPtrA", h_IsBadStringPtrA, 8 },
    { "KERNEL32.dll", "InterlockedIncrement", h_InterlockedIncrement, 4 },
    { "KERNEL32.dll", "InterlockedDecrement", h_InterlockedDecrement, 4 },
    { "KERNEL32.dll", "GetCurrentThread", h_GetCurrentThread, 0 },
    { "KERNEL32.dll", "SetThreadPriority", h_ret_true, 8 },
    { "KERNEL32.dll", "DebugBreak", h_DebugBreak, 0 },
    { "KERNEL32.dll", "GetModuleFileNameA", h_GetModuleFileNameA, 12 },
    { "KERNEL32.dll", "InitializeCriticalSection", h_InitializeCriticalSection, 4 },
    { "KERNEL32.dll", "DeleteCriticalSection", h_DeleteCriticalSection, 4 },
    { "KERNEL32.dll", "EnterCriticalSection", h_EnterCriticalSection, 4 },
    { "KERNEL32.dll", "LeaveCriticalSection", h_LeaveCriticalSection, 4 },
    { "KERNEL32.dll", "CreateEventA", h_CreateEventA, 16 },
    { "KERNEL32.dll", "SetEvent", h_SetEvent, 4 },
    { "KERNEL32.dll", "ResetEvent", h_ResetEvent, 4 },
    { "KERNEL32.dll", "WaitForSingleObject", h_WaitForSingleObject, 8 },
    { "KERNEL32.dll", "WaitForMultipleObjects", h_WaitForMultipleObjects, 16 },
    { "KERNEL32.dll", "Sleep", h_Sleep, -1 },
    { "KERNEL32.dll", "CloseHandle", h_CloseHandle, 4 },
    { "KERNEL32.dll", "CreateFileA", h_CreateFileA, 28 },
    { "KERNEL32.dll", "ReadFile", h_ReadFile, 20 },
    { "KERNEL32.dll", "WriteFile", h_WriteFile, 20 },
    { "KERNEL32.dll", "GetFileSize", h_GetFileSize, 8 },
    { "KERNEL32.dll", "CreateDirectoryA", h_CreateDirectoryA, 8 },
    { "KERNEL32.dll", "FindFirstFileA", h_FindFirstFileA, 8 },
    { "KERNEL32.dll", "FindNextFileA", h_FindNextFileA, 8 },
    { "KERNEL32.dll", "FindClose", h_FindClose, 4 },
    { "MSVCRT.dll", "_beginthreadex", h_beginthreadex, 0 },
    { "ADVAPI32.dll", "GetUserNameA", h_GetUserNameA, 8 },
    { "USER32.dll", "CharUpperBuffA", h_CharUpperBuffA, 8 },
    { "USER32.dll", "CharLowerBuffA", h_CharLowerBuffA, 8 },
    { "USER32.dll", "CharLowerA", h_CharLowerA, 4 },
    { "USER32.dll", "IsCharAlphaA", h_IsCharAlphaA, 4 },
    { "USER32.dll", "IsCharAlphaNumericA", h_IsCharAlphaNumericA, 4 },
    { "USER32.dll", "IsCharUpperA", h_IsCharUpperA, 4 },
    { "USER32.dll", "IsCharLowerA", h_IsCharLowerA, 4 },
    { "USER32.dll", "wsprintfA", h_wsprintfA, 0 },
    { "USER32.dll", "RegisterClassA", h_RegisterClassA, 4 },
    { "USER32.dll", "CreateWindowExA", h_CreateWindowExA, 48 },
    { "USER32.dll", "DestroyWindow", h_DestroyWindow, 4 },
    { "USER32.dll", "IsWindow", h_IsWindow, 4 },
    { "USER32.dll", "GetWindowLongA", h_GetWindowLongA, 8 },
    { "USER32.dll", "SetWindowLongA", h_SetWindowLongA, 12 },
    { "USER32.dll", "PostMessageA", h_PostMessageA, 16 },
    { "USER32.dll", "DefWindowProcA", h_DefWindowProcA, 16 },
    { "USER32.dll", "MessageBoxA", h_MessageBoxA, 16 },
    { "USER32.dll", "DialogBoxParamA", h_DialogBoxParamA, 20 },
    { "USER32.dll", "EndDialog", h_SetText, 8 },
    { "USER32.dll", "SetDlgItemTextA", h_SetText, 12 },
    { "USER32.dll", "SetWindowTextA", h_SetText, 8 },
    { "USER32.dll", "LoadIconA", h_LoadIconA, 8 },
    { "USER32.dll", "LoadCursorA", h_LoadIconA, 8 },
    { "KERNEL32.dll", "DisableThreadLibraryCalls", h_ret_true, 4 },
    { "KERNEL32.dll", "GetWindowsDirectoryA", h_GetWindowsDirectoryA, 8 },
    { "KERNEL32.dll", "OutputDebugStringA", h_OutputDebugStringA, 4 },
    { "KERNEL32.dll", "GlobalAlloc", h_GlobalAlloc, 8 },
    { "KERNEL32.dll", "GlobalFree", h_GlobalFree, 4 },
    { "KERNEL32.dll", "LoadLibraryA", h_LoadLibraryA, 4 },
    { "KERNEL32.dll", "FreeLibrary", h_ret_true, 4 },
    { "KERNEL32.dll", "GetProcAddress", h_ret_zero, 8 },
    { "KERNEL32.dll", "GetThreadPriority", h_ret_zero, 4 },
    { "KERNEL32.dll", "lstrcatA", h_lstrcatA, 8 },
    { "KERNEL32.dll", "InterlockedExchange", h_InterlockedExchange, 8 },
    { "KERNEL32.dll", "CreateThread", h_CreateThread, 24 },
    { "KERNEL32.dll", "OpenFile", h_ret_neg1, 12 },      // only used to save the user lexicon: never
    { "KERNEL32.dll", "_lwrite", h_ret_neg1, 12 },
    { "KERNEL32.dll", "_lclose", h_ret_neg1, 4 },
    { "USER32.dll", "GetMessageA", h_GetMessageA, 16 },
    { "USER32.dll", "PeekMessageA", h_PeekMessageA, -1 },
    { "USER32.dll", "TranslateMessage", h_TranslateMessage, 4 },
    { "USER32.dll", "DispatchMessageA", h_DispatchMessageA, 4 },
    { "USER32.dll", "PostQuitMessage", h_PostQuitMessage, 4 },
    { "USER32.dll", "SendMessageA", h_SendMessageA, 16 },
    { "USER32.dll", "CallWindowProcA", h_CallWindowProcA, 20 },
    { "USER32.dll", "LoadStringA", h_LoadStringA, 16 },
    // dialogs / UI (the engine's control panel): never shown here
    { "USER32.dll", "IsDialogMessageA", h_ret_zero, 8 },
    { "USER32.dll", "LoadBitmapA", h_ret_zero, 8 },
    { "USER32.dll", "CreateDialogParamA", h_ret_zero, 20 },
    { "USER32.dll", "SetWindowPos", h_ret_true, 28 },
    { "USER32.dll", "GetParent", h_ret_zero, 4 },
    { "USER32.dll", "GetDlgItem", h_ret_zero, 8 },
    { "USER32.dll", "GetWindowRect", h_GetRect, 8 },
    { "USER32.dll", "GetClientRect", h_GetRect, 8 },
    { "USER32.dll", "GetDlgItemTextA", h_ret_zero, 16 },
    { "USER32.dll", "GetDlgCtrlID", h_ret_zero, 4 },
    { "USER32.dll", "SetDlgItemInt", h_ret_true, 16 },
    { "USER32.dll", "SendDlgItemMessageA", h_ret_zero, 20 },
    { "USER32.dll", "WinHelpA", h_ret_zero, 16 },
    { "USER32.dll", "IsDlgButtonChecked", h_ret_zero, 8 },
    { "USER32.dll", "CheckDlgButton", h_ret_true, 12 },
    { "USER32.dll", "MoveWindow", h_ret_true, 24 },
    { "USER32.dll", "GetSysColor", h_ret_zero, 4 },
    { "USER32.dll", "FillRect", h_ret_true, 12 },
    { "USER32.dll", "SetCursor", h_ret_zero, 4 },
    { "USER32.dll", "GetWindowTextA", h_ret_zero, 12 },
    { "USER32.dll", "GetDC", h_ret_zero, 4 },
    { "USER32.dll", "ReleaseDC", h_ret_true, 8 },
    { "USER32.dll", "EnableWindow", h_ret_zero, 8 },
    { "USER32.dll", "GetKeyState", h_ret_zero, 4 },
    { "USER32.dll", "SetFocus", h_ret_zero, 4 },
    { "GDI32.dll", "CreateSolidBrush", h_ret_zero, 4 },
    { "GDI32.dll", "DeleteDC", h_ret_true, 4 },
    { "GDI32.dll", "BitBlt", h_ret_zero, 36 },
    { "GDI32.dll", "DeleteObject", h_ret_true, 4 },
    { "GDI32.dll", "CreateCompatibleDC", h_ret_zero, 4 },
    { "GDI32.dll", "CreateFontIndirectA", h_ret_zero, 4 },
    { "GDI32.dll", "SelectObject", h_ret_zero, 8 },
    { "GDI32.dll", "EnumFontFamiliesExA", h_ret_zero, 20 },
    { "GDI32.dll", "ExtTextOutA", h_ret_zero, 32 },
    { "GDI32.dll", "GetObjectA", h_ret_zero, 12 },
    { "GDI32.dll", "SetBkColor", h_ret_zero, 8 },
    { "GDI32.dll", "SetTextColor", h_ret_zero, 8 },
    { "OLE32.dll", "CoTaskMemAlloc", h_CoTaskMemAlloc, 4 },
    { "OLE32.dll", "CoTaskMemFree", h_CoTaskMemFree, 4 },
    { "OLE32.dll", "CoTaskMemRealloc", h_CoTaskMemRealloc, 8 },
    { "OLE32.dll", "StringFromCLSID", h_StringFromCLSID, 8 },
    { "OLE32.dll", "CoGetMalloc", h_CoGetMalloc, 8 },
    { "OLE32.dll", "CoInitialize", h_ret_zero, 4 },
    { "OLE32.dll", "CoUninitialize", h_ret_zero, 0 },
    { "OLE32.dll", "CoCreateInstance", h_CoCreateInstance, 20 },
};

void emu_register_winapi_apis(void) { emu_add_apis(win_apis, (int)(sizeof win_apis / sizeof win_apis[0])); }
