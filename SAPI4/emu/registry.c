// ADVAPI32 registry, in memory and per Emu: nothing is read from or written to the host. An engine
// that expects to have been installed fills it by running its own DllRegisterServer (what the
// installer's regsvr32 step did), then reads back what it wrote.
#include "emu_internal.h"
#include <strings.h>

#define A(i) x86_arg(c, (i))

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259

typedef struct RegVal { char name[256]; uint32_t type, len; uint8_t *data; } RegVal;
typedef struct RegKey {
    char name[256];
    struct RegKey *parent, *child, *next;
    RegVal *vals; int nvals;
} RegKey;

static RegKey *key_new(RegKey *parent, const char *name, size_t n) {
    RegKey *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    if (n > sizeof k->name - 1) n = sizeof k->name - 1;
    memcpy(k->name, name, n);
    k->parent = parent;
    if (parent) { k->next = parent->child; parent->child = k; }
    return k;
}
static void key_free(RegKey *k) {
    while (k->child) { RegKey *ch = k->child; k->child = ch->next; key_free(ch); }
    for (int i = 0; i < k->nvals; i++) free(k->vals[i].data);
    free(k->vals);
    free(k);
}
static void key_unlink(RegKey *k) {
    if (!k->parent) return;
    RegKey **pp = &k->parent->child;
    while (*pp && *pp != k) pp = &(*pp)->next;
    if (*pp) *pp = k->next;
}

void emu_registry_cleanup(Emu *e) {
    for (int i = 0; i < 4; i++) if (e->reg_roots[i]) { key_free(e->reg_roots[i]); e->reg_roots[i] = NULL; }
}

// HKEY -> key; predefined roots 0x80000000..0x80000003 are created on first use
static RegKey *key_of(Emu *e, uint32_t h) {
    if (h >= 0x80000000u && h <= 0x80000003u) {
        int i = (int)(h - 0x80000000u);
        if (!e->reg_roots[i]) e->reg_roots[i] = key_new(NULL, "", 0);
        return e->reg_roots[i];
    }
    Handle *p = emu_handle(e, h);
    return p && p->type == H_KEY ? (RegKey *)p->obj : NULL;
}

// walks "a\b\c" below k; create = make missing keys
static RegKey *key_walk(RegKey *k, const char *path, int create) {
    while (k && path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        const char *end = strchr(path, '\\');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        RegKey *ch = k->child;
        while (ch && !(strlen(ch->name) == n && strncasecmp(ch->name, path, n) == 0)) ch = ch->next;
        if (!ch && create) ch = key_new(k, path, n);
        k = ch;
        path += n;
    }
    return k;
}

static uint32_t key_handle(Emu *e, RegKey *k) {
    uint32_t h = emu_new_handle(e, H_KEY);
    emu_handle(e, h)->obj = k;
    return h;
}

static RegVal *val_find(RegKey *k, const char *name) {
    if (!name) name = "";
    for (int i = 0; i < k->nvals; i++) if (!strcasecmp(k->vals[i].name, name)) return &k->vals[i];
    return NULL;
}

static void open_key(Emu *e, X86 *c, uint32_t hkey, uint32_t sub, uint32_t out, int create, uint32_t disp) {
    RegKey *k = key_of(e, hkey);
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    const char *path = sub ? emu_str(e, sub) : "";
    RegKey *found = key_walk(k, path, 0);
    int existed = found != NULL;
    if (!found && create) found = key_walk(k, path, 1);
    if (emu_trace_api) EMU_LOG("[reg] %s '%s' -> %s\n", create ? "create" : "open", path, found ? (existed ? "ok" : "created") : "not found");
    if (!found) { if (out) wr32(c, out, 0); E_RET(ERROR_FILE_NOT_FOUND); return; }
    if (out) wr32(c, out, key_handle(e, found));
    if (disp) wr32(c, disp, existed ? 2 : 1);   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    E_RET(ERROR_SUCCESS);
}

static void h_RegOpenKeyA(Emu *e, X86 *c) { open_key(e, c, A(0), A(1), A(2), 0, 0); }
static void h_RegOpenKeyExA(Emu *e, X86 *c) { open_key(e, c, A(0), A(1), A(4), 0, 0); }
static void h_RegCreateKeyA(Emu *e, X86 *c) { open_key(e, c, A(0), A(1), A(2), 1, 0); }
static void h_RegCreateKeyExA(Emu *e, X86 *c) { open_key(e, c, A(0), A(1), A(7), 1, A(8)); }
static void h_RegCloseKey(Emu *e, X86 *c) {
    uint32_t h = A(0);
    if (h >= 0x80000000u) { E_RET(ERROR_SUCCESS); return; }
    Handle *p = emu_handle(e, h);
    if (!p || p->type != H_KEY) { E_RET(ERROR_INVALID_HANDLE); return; }
    emu_close_handle(e, h);
    E_RET(ERROR_SUCCESS);
}
static void h_RegQueryValueExA(Emu *e, X86 *c) {
    RegKey *k = key_of(e, A(0));
    uint32_t typep = A(3), data = A(4), lenp = A(5);
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    const char *name = A(1) ? emu_str(e, A(1)) : "";
    RegVal *v = val_find(k, name);
    if (emu_trace_api) EMU_LOG("[reg] query '%s' in '%s' -> %s\n", name, k->name, v ? "found" : "not found");
    if (!v) { E_RET(ERROR_FILE_NOT_FOUND); return; }
    if (typep) wr32(c, typep, v->type);
    uint32_t cap = lenp ? rd32(c, lenp) : 0;
    if (lenp) wr32(c, lenp, v->len);
    if (!data) { E_RET(ERROR_SUCCESS); return; }
    if (!lenp || cap < v->len) { E_RET(ERROR_MORE_DATA); return; }
    if (v->len) memcpy(emu_ptr(e, data, v->len), v->data, v->len);
    E_RET(ERROR_SUCCESS);
}
static void h_RegSetValueExA(Emu *e, X86 *c) {
    RegKey *k = key_of(e, A(0));
    uint32_t type = A(3), data = A(4), len = A(5);
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    const char *name = A(1) ? emu_str(e, A(1)) : "";
    if (len > 65536) { E_RET(87); return; }
    RegVal *v = val_find(k, name);
    if (!v) {
        RegVal *nv = realloc(k->vals, sizeof *nv * (size_t)(k->nvals + 1));
        if (!nv) { E_RET(8); return; }
        k->vals = nv;
        v = &k->vals[k->nvals++];
        memset(v, 0, sizeof *v);
        snprintf(v->name, sizeof v->name, "%s", name);
    }
    free(v->data);
    v->data = malloc(len ? len : 1);
    if (len) memcpy(v->data, emu_ptr(e, data, len), len);
    v->len = len;
    v->type = type;
    if (emu_trace_api) EMU_LOG("[reg] set '%s' in '%s' (type %u, %u bytes)\n", name, k->name, type, len);
    E_RET(ERROR_SUCCESS);
}
static void h_RegDeleteValueA(Emu *e, X86 *c) {
    RegKey *k = key_of(e, A(0));
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    RegVal *v = val_find(k, A(1) ? emu_str(e, A(1)) : "");
    if (!v) { E_RET(ERROR_FILE_NOT_FOUND); return; }
    free(v->data);
    int i = (int)(v - k->vals);
    memmove(&k->vals[i], &k->vals[i + 1], sizeof *v * (size_t)(k->nvals - i - 1));
    k->nvals--;
    E_RET(ERROR_SUCCESS);
}
static void h_RegDeleteKeyA(Emu *e, X86 *c) {
    RegKey *k = key_of(e, A(0));
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    RegKey *t = key_walk(k, A(1) ? emu_str(e, A(1)) : "", 0);
    if (!t || t == k || !t->parent) { E_RET(ERROR_FILE_NOT_FOUND); return; }
    // open handles to the deleted subtree: detach them (Win32 makes them invalid)
    for (int i = 0; i < EMU_MAX_HANDLES; i++) {
        Handle *p = &e->handles[i];
        if (p->type != H_KEY) continue;
        for (RegKey *q = p->obj; q; q = q->parent) if (q == t) { p->type = H_FREE; break; }
    }
    key_unlink(t);
    key_free(t);
    E_RET(ERROR_SUCCESS);
}
static void h_RegQueryInfoKeyA(Emu *e, X86 *c) {
    RegKey *k = key_of(e, A(0));
    if (!k) { E_RET(ERROR_INVALID_HANDLE); return; }
    uint32_t nsub = 0, maxsub = 0, maxvn = 0, maxvl = 0;
    for (RegKey *ch = k->child; ch; ch = ch->next) { nsub++; if (strlen(ch->name) > maxsub) maxsub = (uint32_t)strlen(ch->name); }
    for (int i = 0; i < k->nvals; i++) {
        if (strlen(k->vals[i].name) > maxvn) maxvn = (uint32_t)strlen(k->vals[i].name);
        if (k->vals[i].len > maxvl) maxvl = k->vals[i].len;
    }
    if (A(1) && A(2) && rd32(c, A(2))) wr8(c, A(1), 0);
    if (A(2)) wr32(c, A(2), 0);
    if (A(4)) wr32(c, A(4), nsub);
    if (A(5)) wr32(c, A(5), maxsub);
    if (A(6)) wr32(c, A(6), 0);
    if (A(7)) wr32(c, A(7), (uint32_t)k->nvals);
    if (A(8)) wr32(c, A(8), maxvn);
    if (A(9)) wr32(c, A(9), maxvl);
    if (A(10)) wr32(c, A(10), 0);
    if (A(11)) wr64(c, A(11), 0);
    E_RET(ERROR_SUCCESS);
}

static const ApiDef reg_apis[] = {
    { "ADVAPI32.dll", "RegOpenKeyA", h_RegOpenKeyA, 12 },
    { "ADVAPI32.dll", "RegOpenKeyExA", h_RegOpenKeyExA, 20 },
    { "ADVAPI32.dll", "RegCreateKeyA", h_RegCreateKeyA, 12 },
    { "ADVAPI32.dll", "RegCreateKeyExA", h_RegCreateKeyExA, 36 },
    { "ADVAPI32.dll", "RegCloseKey", h_RegCloseKey, 4 },
    { "ADVAPI32.dll", "RegQueryValueExA", h_RegQueryValueExA, 24 },
    { "ADVAPI32.dll", "RegSetValueExA", h_RegSetValueExA, 24 },
    { "ADVAPI32.dll", "RegDeleteValueA", h_RegDeleteValueA, 8 },
    { "ADVAPI32.dll", "RegDeleteKeyA", h_RegDeleteKeyA, 8 },
    { "ADVAPI32.dll", "RegQueryInfoKeyA", h_RegQueryInfoKeyA, 48 },
};
void emu_register_registry_apis(void) { emu_add_apis(reg_apis, (int)(sizeof reg_apis / sizeof reg_apis[0])); }
