// OLE32 structured storage, read-only: StgOpenStorage + IStorage / IStream host objects over a
// small compound-file (MS-CFB v3/v4) reader.
#include "emu_internal.h"
#include <fcntl.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define A(i) x86_arg(c, (i))

typedef struct {
    char name[64];      // UTF-16 name folded to Latin-1
    int type;           // 1 storage, 2 stream, 5 root
    uint32_t left, right, child, start;
    uint64_t size;
} CfbEntry;

typedef struct Cfb {
    uint8_t *data; size_t len;          // the whole file, mapped read-only (clean, purgeable pages)
    uint32_t ssz, mssz, cutoff;
    uint32_t *fat; uint32_t nfat;
    uint32_t *mfat; uint32_t nmfat;
    CfbEntry *ents; uint32_t nents;
    uint8_t *ministream; size_t ministream_len;
    int refs;
} Cfb;

#define ENDOFCHAIN 0xFFFFFFFAu
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint8_t *cfb_sector(Cfb *f, uint32_t s) {
    size_t off = (size_t)(s + 1) * f->ssz;
    if (off + f->ssz > f->len) return NULL;
    return f->data + off;
}
// read a regular-FAT chain into a new buffer
static uint8_t *cfb_read_chain(Cfb *f, uint32_t s, size_t size, size_t *outlen) {
    size_t cap = (size ? size : 0) + f->ssz, n = 0;
    uint8_t *buf = malloc(cap);
    uint32_t guard = 0;
    while (s < ENDOFCHAIN && guard++ < 10000000) {
        uint8_t *sec = cfb_sector(f, s);
        if (!sec || s >= f->nfat) break;
        if (n + f->ssz > cap) { cap = (n + f->ssz) * 2; buf = realloc(buf, cap); }
        memcpy(buf + n, sec, f->ssz);
        n += f->ssz;
        s = f->fat[s];
    }
    if (size && n > size) n = size;
    *outlen = n;
    return buf;
}

static Cfb *cfb_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 512) { close(fd); return NULL; }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return NULL;
    Cfb *f = calloc(1, sizeof *f);
    if (!f) { munmap(map, (size_t)st.st_size); return NULL; }
    f->data = map;
    f->len = (size_t)st.st_size;
    static const uint8_t sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    if (f->len < 512 || memcmp(f->data, sig, 8)) goto bad;
    f->ssz = 1u << le16(f->data + 30);
    f->mssz = 1u << le16(f->data + 32);
    uint32_t nfatsec = le32(f->data + 44), dir0 = le32(f->data + 48);
    f->cutoff = le32(f->data + 56);
    uint32_t mfat0 = le32(f->data + 60), difat0 = le32(f->data + 68), ndifat = le32(f->data + 72);
    // DIFAT
    uint32_t *difat = malloc(sizeof(uint32_t) * (109 + (size_t)ndifat * (f->ssz / 4)));
    uint32_t nd = 0;
    for (int i = 0; i < 109; i++) difat[nd++] = le32(f->data + 76 + 4 * i);
    for (uint32_t s = difat0, k = 0; s < ENDOFCHAIN && k < ndifat; k++) {
        uint8_t *sec = cfb_sector(f, s);
        if (!sec) break;
        for (uint32_t i = 0; i + 1 < f->ssz / 4; i++) difat[nd++] = le32(sec + 4 * i);
        s = le32(sec + f->ssz - 4);
    }
    f->nfat = nfatsec * (f->ssz / 4);
    f->fat = malloc(sizeof(uint32_t) * f->nfat);
    for (uint32_t i = 0; i < nfatsec; i++) {
        uint8_t *sec = cfb_sector(f, difat[i]);
        if (!sec) { free(difat); goto bad; }
        for (uint32_t j = 0; j < f->ssz / 4; j++) f->fat[i * (f->ssz / 4) + j] = le32(sec + 4 * j);
    }
    free(difat);
    size_t dlen;
    uint8_t *dir = cfb_read_chain(f, dir0, 0, &dlen);
    f->nents = (uint32_t)(dlen / 128);
    f->ents = calloc(f->nents, sizeof(CfbEntry));
    for (uint32_t i = 0; i < f->nents; i++) {
        uint8_t *e = dir + 128 * i;
        CfbEntry *x = &f->ents[i];
        uint16_t nl = le16(e + 64);
        int nch = nl >= 2 ? (nl - 2) / 2 : 0;
        if (nch > 63) nch = 63;
        for (int k = 0; k < nch; k++) x->name[k] = (char)le16(e + 2 * k);
        x->name[nch] = 0;
        x->type = e[66];
        x->left = le32(e + 68); x->right = le32(e + 72); x->child = le32(e + 76);
        x->start = le32(e + 116);
        x->size = le32(e + 120);
    }
    free(dir);
    // mini FAT + mini stream
    size_t mlen = 0;
    if (mfat0 < ENDOFCHAIN) {
        uint8_t *mf = cfb_read_chain(f, mfat0, 0, &mlen);
        f->nmfat = (uint32_t)(mlen / 4);
        f->mfat = malloc(sizeof(uint32_t) * (f->nmfat ? f->nmfat : 1));
        for (uint32_t i = 0; i < f->nmfat; i++) f->mfat[i] = le32(mf + 4 * i);
        free(mf);
    }
    if (f->nents && f->ents[0].start < ENDOFCHAIN) f->ministream = cfb_read_chain(f, f->ents[0].start, (size_t)f->ents[0].size, &f->ministream_len);
    f->refs = 1;
    return f;
bad:
    munmap(f->data, f->len); free(f->fat); free(f);
    return NULL;
}
static void cfb_release(Cfb *f) {
    if (--f->refs > 0) return;
    munmap(f->data, f->len); free(f->fat); free(f->mfat); free(f->ents); free(f->ministream); free(f);
}
static uint8_t *cfb_stream(Cfb *f, CfbEntry *e, size_t *len) {
    if (e->size < f->cutoff) {
        uint8_t *buf = malloc(e->size ? (size_t)e->size : 1);
        size_t n = 0;
        uint32_t s = e->start, guard = 0;
        while (s < ENDOFCHAIN && n < e->size && guard++ < 10000000 && s < f->nmfat) {
            size_t off = (size_t)s * f->mssz, k = f->mssz;
            if (n + k > e->size) k = (size_t)e->size - n;
            if (off + k > f->ministream_len) break;
            memcpy(buf + n, f->ministream + off, k);
            n += k;
            s = f->mfat[s];
        }
        *len = n;
        return buf;
    }
    return cfb_read_chain(f, e->start, (size_t)e->size, len);
}
// find a child of storage entry `parent` by (case-insensitive) name: walk the sibling tree
static int cfb_find(Cfb *f, uint32_t idx, const char *name) {
    uint32_t stack[256]; int sp = 0;
    if (idx < f->nents) stack[sp++] = idx;
    while (sp) {
        uint32_t i = stack[--sp];
        if (i >= f->nents) continue;
        if (!strcasecmp(f->ents[i].name, name)) return (int)i;
        if (sp < 254) { stack[sp++] = f->ents[i].left; stack[sp++] = f->ents[i].right; }
    }
    return -1;
}

// ------------------------------------------------------------------ host objects
// Host objects live in a per-Emu table (Emu.ole_objs); the guest object is [vtable][table index].
typedef struct OleObj { int kind; int refs; Cfb *f; uint32_t dir; uint8_t *data; size_t len, pos; char name[64]; } OleObj; // kind 1 storage, 2 stream

static OleObj *obj_of(Emu *e, X86 *c, uint32_t guest) {
    uint32_t id = rd32(c, guest + 4);
    if (id >= 512 || !e->ole_objs[id]) x86_fault(c, "bad OLE object %08x", guest);
    return e->ole_objs[id];
}
static uint32_t new_obj(Emu *e, OleObj *o) {
    int id = -1;
    for (int i = 1; i < 512; i++) if (!e->ole_objs[i]) { id = i; break; }
    if (id < 0) x86_fault(&e->cpu, "too many OLE objects");
    e->ole_objs[id] = o;
    uint32_t g = emu_malloc(e, 8);
    wr32(&e->cpu, g, o->kind == 1 ? e->ole_stg_vt : e->ole_stm_vt);
    wr32(&e->cpu, g + 4, (uint32_t)id);
    return g;
}
static void wname(Emu *e, uint32_t a, char *out, size_t n) {
    size_t len;
    const uint16_t *w = emu_wstr(e, a, &len);
    size_t k = 0;
    for (; k < len && k < n - 1; k++) out[k] = (char)w[k];
    out[k] = 0;
}

static void o_QI(Emu *e, X86 *c) { (void)e; wr32(c, A(2), A(0)); obj_of(e, c, A(0))->refs++; E_RET(0); }
static void o_AddRef(Emu *e, X86 *c) { E_RET(++obj_of(e, c, A(0))->refs); }
static void o_Release(Emu *e, X86 *c) {
    OleObj *o = obj_of(e, c, A(0));
    uint32_t r = (uint32_t)--o->refs;
    if (!r) {
        uint32_t id = rd32(c, A(0) + 4);
        if (o->kind == 2) free(o->data);
        cfb_release(o->f);
        free(o);
        e->ole_objs[id] = NULL;
        emu_free(e, A(0));
    }
    E_RET(r);
}
static void o_notimpl(Emu *e, X86 *c) {
    (void)e;
    uint32_t idx = (c->s.eip - EMU_THUNK_BASE) / 4;
    (void)idx;
    EMU_LOG("[ole] %s not implemented\n", e->hosts[idx].name);
    E_RET(0x80004001u);
}
static void stg_OpenStream(Emu *e, X86 *c) {
    OleObj *s = obj_of(e, c, A(0));
    char name[64];
    wname(e, A(1), name, sizeof name);
    int i = cfb_find(s->f, s->f->ents[s->dir].child, name);
    if (i < 0 || s->f->ents[i].type != 2) {
        if (emu_trace_api) EMU_LOG("[ole] OpenStream('%s') -> not found\n", name);
        wr32(c, A(5), 0);
        E_RET(0x80030002u);
        return;
    }
    OleObj *o = calloc(1, sizeof *o);
    o->kind = 2; o->refs = 1; o->f = s->f; s->f->refs++;
    o->data = cfb_stream(s->f, &s->f->ents[i], &o->len);
    snprintf(o->name, sizeof o->name, "%s", name);
    wr32(c, A(5), new_obj(e, o));
    E_RET(0);
}
static void stg_OpenStorage(Emu *e, X86 *c) {
    OleObj *s = obj_of(e, c, A(0));
    char name[64];
    wname(e, A(1), name, sizeof name);
    int i = cfb_find(s->f, s->f->ents[s->dir].child, name);
    if (i < 0 || s->f->ents[i].type != 1) { wr32(c, A(6), 0); E_RET(0x80030002u); return; }
    OleObj *o = calloc(1, sizeof *o);
    o->kind = 1; o->refs = 1; o->f = s->f; s->f->refs++; o->dir = (uint32_t)i;
    snprintf(o->name, sizeof o->name, "%s", name);
    wr32(c, A(6), new_obj(e, o));
    E_RET(0);
}
static void fill_stat(Emu *e, X86 *c, uint32_t st, int type, uint64_t size) {
    memset(emu_ptr(e, st, 72), 0, 72);
    wr32(c, st + 4, (uint32_t)type);
    wr64(c, st + 8, size);
}
static void stg_Stat(Emu *e, X86 *c) { obj_of(e, c, A(0)); fill_stat(e, c, A(1), 1, 0); E_RET(0); }
static void stm_Read(Emu *e, X86 *c) {
    OleObj *o = obj_of(e, c, A(0));
    uint32_t n = A(2);
    size_t avail = o->pos < o->len ? o->len - o->pos : 0;
    if (n > avail) n = (uint32_t)avail;
    if (n) memcpy(emu_ptr(e, A(1), n), o->data + o->pos, n);
    o->pos += n;
    if (A(3)) wr32(c, A(3), n);
    E_RET(0);  // S_OK even on short read (as OLE does)
}
static void stm_Seek(Emu *e, X86 *c) {
    OleObj *o = obj_of(e, c, A(0));
    int64_t move = (int64_t)(((uint64_t)A(2) << 32) | A(1));
    uint32_t origin = A(3), out = A(4);
    int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)o->pos : (int64_t)o->len;
    int64_t np = base + move;
    if (np < 0) { E_RET(0x80030019u); return; } // STG_E_INVALIDFUNCTION
    o->pos = (size_t)np;
    if (out) wr64(c, out, (uint64_t)np);
    E_RET(0);
}
static void stm_Stat(Emu *e, X86 *c) { OleObj *o = obj_of(e, c, A(0)); fill_stat(e, c, A(1), 2, o->len); E_RET(0); }
static void stm_Write(Emu *e, X86 *c) { (void)e; if (A(3)) wr32(c, A(3), 0); E_RET(0x80030005u); } // STG_E_ACCESSDENIED

static void h_StgOpenStorage(Emu *e, X86 *c) {
    char name[300], host[1100];
    wname(e, A(0), name, sizeof name);
    uint32_t pp = A(5);
    if (pp) wr32(c, pp, 0);
    if (!emu_host_path(e, name, host, sizeof host, 1)) { EMU_LOG("[ole] StgOpenStorage('%s') -> not found\n", name); E_RET(0x80030002u); return; }
    Cfb *f = cfb_open(host);
    if (!f) { EMU_LOG("[ole] StgOpenStorage('%s') -> not a compound file\n", name); E_RET(0x80030050u); return; } // STG_E_FILEALREADYEXISTS-ish
    OleObj *o = calloc(1, sizeof *o);
    o->kind = 1; o->refs = 1; o->f = f; o->dir = 0;
    snprintf(o->name, sizeof o->name, "%s", name);
    if (emu_trace_api) EMU_LOG("[ole] StgOpenStorage('%s') ok, %u entries\n", name, f->nents);
    wr32(c, pp, new_obj(e, o));
    E_RET(0);
}

void emu_register_ole(Emu *e) {
    static const EmuMethod stg[] = {
        { "IStorage::QueryInterface", o_QI, 2 }, { "IStorage::AddRef", o_AddRef, 0 }, { "IStorage::Release", o_Release, 0 },
        { "IStorage::CreateStream", o_notimpl, 5 }, { "IStorage::OpenStream", stg_OpenStream, 5 },
        { "IStorage::CreateStorage", o_notimpl, 5 }, { "IStorage::OpenStorage", stg_OpenStorage, 6 },
        { "IStorage::CopyTo", o_notimpl, 4 }, { "IStorage::MoveElementTo", o_notimpl, 4 }, { "IStorage::Commit", o_notimpl, 1 },
        { "IStorage::Revert", o_notimpl, 0 }, { "IStorage::EnumElements", o_notimpl, 4 }, { "IStorage::DestroyElement", o_notimpl, 1 },
        { "IStorage::RenameElement", o_notimpl, 2 }, { "IStorage::SetElementTimes", o_notimpl, 4 }, { "IStorage::SetClass", o_notimpl, 1 },
        { "IStorage::SetStateBits", o_notimpl, 2 }, { "IStorage::Stat", stg_Stat, 2 },
    };
    static const EmuMethod stm[] = {
        { "IStream::QueryInterface", o_QI, 2 }, { "IStream::AddRef", o_AddRef, 0 }, { "IStream::Release", o_Release, 0 },
        { "IStream::Read", stm_Read, 3 }, { "IStream::Write", stm_Write, 3 }, { "IStream::Seek", stm_Seek, 4 },
        { "IStream::SetSize", o_notimpl, 2 }, { "IStream::CopyTo", o_notimpl, 5 }, { "IStream::Commit", o_notimpl, 1 },
        { "IStream::Revert", o_notimpl, 0 }, { "IStream::LockRegion", o_notimpl, 5 }, { "IStream::UnlockRegion", o_notimpl, 5 },
        { "IStream::Stat", stm_Stat, 2 }, { "IStream::Clone", o_notimpl, 1 },
    };
    e->ole_stg_vt = emu_vtable(e, stg, 18);
    e->ole_stm_vt = emu_vtable(e, stm, 14);
}

void emu_register_ole_apis(void) {
    static const ApiDef apis[] = { { "OLE32.dll", "StgOpenStorage", h_StgOpenStorage, 24 } };
    emu_add_apis(apis, 1);
}

// frees the host side of every OLE object still open (emu_destroy)
void emu_ole_cleanup(Emu *e) {
    for (int i = 0; i < 512; i++) {
        OleObj *o = e->ole_objs[i];
        if (!o) continue;
        if (o->kind == 2) free(o->data);
        cfb_release(o->f);
        free(o);
        e->ole_objs[i] = NULL;
    }
}
