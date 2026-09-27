// The engine's voices: enumeration of the voice and character files, their mode descriptions, and
// the table of loaded voices.
#include "voices.h"
#include "crt_vc.h"
#include "vcrt.h"
#include <stdio.h>
#include <string.h>

LAYOUT(VoiceHdr, gender, 0x8d2);
LAYOUT(VoiceHdr, features, 0x8dc);
LAYOUT(VoiceHdr, variants, 0x8f4);
LAYOUT(VoiceCfg, name, 0x90c);
LAYOUT(TtsModeInfo, language, 0x644);
LAYOUT(TtsModeInfo, gender, 0xade);
LAYOUT(TtsModeInfo, engine_features, 0xaec);
_Static_assert(sizeof(VoiceHdr) == 0x8f8, "VoiceHdr");
_Static_assert(sizeof(VoiceCfg) == 0xb18, "VoiceCfg");
_Static_assert(sizeof(TtsModeInfo) == 0xaf0, "TtsModeInfo");
#ifdef DECOMP_HOOK
_Static_assert(sizeof(ModeTable) == 0x24, "ModeTable");
#endif

#define CS DLLVAR(void, 0x63738ba8)

ModeList *list_ctor(ModeList *l) {
    l->count = 0;
    l->items = 0;
    l->cap = 0;
    return l;
}

uint32_t list_count(const ModeList *l) { return l->count; }

int32_t list_grow(ModeList *l, uint32_t n) {
    uint32_t bytes = n * (uint32_t)sizeof(ListItem);
    if (bytes <= l->cap) return 1;
    bytes += bytes >> 1;
    if (l->items) {
        ListItem *p = vc_realloc(GP(void, l->items), bytes);
        if (!p) return 0;
        GPSET(l->items, p);
        l->cap = bytes;
        return 1;
    }
    ListItem *p = vc_malloc(bytes);
    GPSET(l->items, p);
    if (p) l->cap = bytes;
    return p != NULL;
}

int32_t list_add(ModeList *l, const void *data, uint32_t size) {
    if (!data || !size) return 0;
    if (!list_grow(l, l->count + 1)) return 0;
    void *p = vc_malloc(size);
    if (!p) return 0;
    memcpy(p, data, size);
    ListItem *it = GP(ListItem, l->items) + l->count;
    GPSET(it->data, p);
    it->size = size;
    l->count++;
    return 1;
}

GPTR(void) list_item(const ModeList *l, uint32_t i) {
    if (i >= l->count) return 0;
    return GP(const ListItem, l->items)[i].data;
}

void wstr_copy(uint16_t *dst, const uint16_t *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

// the path as the wide string StgOpenStorage takes (MultiByteToWideChar, ANSI code page)
static uint16_t *wide_path(const char *path) {
    uint16_t *w = vc_malloc(strlen(path) * 2 + 2);
    vc_MultiByteToWideChar_cp1252((const uint8_t *)path, (int)strlen(path) + 1, w);
    return w;
}

static int32_t open_storage(const char *path, void **stg) {
    uint16_t *w = wide_path(path);
    vc_EnterCriticalSection(CS);
    int32_t hr = vc_StgOpenStorage(w, 0x20, stg);
    vc_LeaveCriticalSection(CS);
    vc_free(w);
    return hr;
}

static void wname(uint16_t *out, const char *pfx, int32_t n) {
    char t[32];
    snprintf(t, sizeof t, "%s%d", pfx, n);
    int k = 0;
    for (; t[k]; k++) out[k] = (uint8_t)t[k];
    out[k] = 0;
}

int32_t voice_read_header(const char *path, VoiceHdr *hdr) {
    void *stg = NULL, *stm = NULL;
    int32_t r = 0;
    if (open_storage(path, &stg) < 0) return (int32_t)0x80030002;
    if (vc_stg_open_stream(stg, DLLVAR(const uint16_t, 0x6371ef30), &stm) < 0) return (int32_t)0x80030002;  // L"VcHeader" (the storage stays open)
    uint32_t got = 0;
    vc_stm_read(stm, hdr, 0x8f4, &got);
    const uint8_t *f178 = DLLVAR(const uint8_t, 0x6369f178), *f168 = DLLVAR(const uint8_t, 0x6369f168),
                  *f188 = DLLVAR(const uint8_t, 0x6369f188);
    if (got != 0x8f4) {
        r = (int32_t)0x80030002;
    } else if (vc_memcmp(hdr, f178, 16) == 0 || vc_memcmp(hdr, f168, 16) == 0 || vc_memcmp(hdr, f188, 16) == 0) {
        if (vc_memcmp(hdr, f188, 16) == 0) {        // the newer format carries the two fields
            vc_stm_read(stm, &hdr->variants, 2, &got);
            hdr->_8f6 = 0;
            vc_stm_read(stm, &hdr->_8f6, 2, &got);
        } else {
            hdr->variants = 0;
            hdr->_8f6 = 0;
        }
    } else {
        r = (int32_t)0x80030002;
    }
    vc_com_release(stm);
    vc_EnterCriticalSection(CS);
    vc_com_release(stg);
    vc_LeaveCriticalSection(CS);
    return r;
}

void mode_info(const VoiceHdr *h, TtsModeInfo *out) {
    memset(out, 0, 0xaf0);
    memcpy(out->engine_id, DLLVAR(const uint8_t, 0x6369f1b8), 16);
    wstr_copy(out->mfg, h->mfg);
    wstr_copy(out->product, DLLVAR(const uint16_t, 0x6371ef44));   // L"Microsoft Speech Synthesis Engine"
    memcpy(out->mode_id, h->mode_id, 16);
    wstr_copy(out->mode_name, h->product);
    memcpy(out->language, h->language, 0x82);
    wstr_copy(out->speaker, h->speaker);
    wstr_copy(out->style, h->style);
    out->gender = h->gender;
    out->age = h->age;
    out->features = h->features | 0x1a0ff;
    out->interfaces = 0x3f;
}

void mode_info_cfg(VoiceCfg *cfg, const VoiceHdr *voice, TtsModeInfo *out) {
    VoiceHdr t;
    memcpy(&t, voice, 0x8f8);
    memcpy(t.mode_id, cfg->mode_id, 16);
    memcpy(t.format, DLLVAR(const uint8_t, 0x6369f198), 16);
    if (cfg->h.mfg[0]) memcpy(t.mfg, cfg->h.mfg, 0x20c);
    if (cfg->h.product[0]) memcpy(t.product, cfg->h.product, 0x20c);
    if (*(const uint16_t *)(const void *)cfg->h.language != 0xffff) memcpy(t.language, cfg->h.language, 0x82);
    if (cfg->h.speaker[0]) memcpy(t.speaker, cfg->h.speaker, 0x20c);
    if (cfg->h.style[0]) memcpy(t.style, cfg->h.style, 0x20c);
    if (cfg->h.gender != 0xffff) t.gender = cfg->h.gender;
    if (cfg->h.age != 0xffff) t.age = cfg->h.age;
    if (cfg->h.rate != 0xffff) t.rate = cfg->h.rate;
    if (cfg->h.pitch != 0xffff) t.pitch = cfg->h.pitch;
    if (cfg->h.features != 0xffffffffu) t.features = cfg->h.features;
    uint8_t fmt[16], id[16];
    memcpy(fmt, cfg->h.format, 16);
    memcpy(id, cfg->h.mode_id, 16);
    t._8f6 = cfg->h._8f6;       // (a dead store in the original: t is used only up to features)
    memcpy(cfg, &t, 0x8f4);
    memcpy(cfg->h.mode_id, id, 16);
    memcpy(cfg->h.format, fmt, 16);
    mode_info(&t, out);
}

int32_t cfg_check_40(uint32_t key, uint32_t len, const void *data) {
    (void)len; (void)data;
    return (key & 0xffffff) == 5;
}
int32_t cfg_check_20(uint32_t key, uint32_t len, const void *data) {
    (void)key; (void)len; (void)data;
    return 0;
}
int32_t cfg_check_10(uint32_t key, uint32_t len, const void *data) {
    (void)len; (void)data;
    uint32_t k = key & 0xffffff;
    return k != 0 && (k <= 4 || k == 6);
}

int32_t cfg_read(int16_t idx, int16_t count, const char *path, VoiceCfg *cfg, const ModeList *infos, GPTR(void) *stgp) {
    void *stg = GPN(void, *stgp);
    uint32_t cap = 0;
    uint8_t *buf = NULL;
    int32_t r;
    if (idx > count) {
        if (stg) vc_com_release(stg);
        *stgp = 0;
        return (int32_t)0x80030002;
    }
    if (!stg && open_storage(path, &stg) < 0) return (int32_t)0x80030002;
    uint16_t name[20];
    wname(name, "CFG", idx);
    void *stm = NULL;
    if (vc_stg_open_stream(stg, name, &stm) < 0) return (int32_t)0x80030002;   // (the storage is not kept)
    uint32_t got;
    cfg->h._8f6 = 0;
    vc_stm_read(stm, cfg, 0xb18, &got);
    if (got != 0xb18 || cfg->version != 1) {
        r = (int32_t)0x80030002;
        goto out;
    }
    uint32_t found;
    r = mode_find(infos, cfg->h.mode_id, &found);
    if (r < 0) goto out;
    uint32_t nrec;
    int32_t hr = vc_stm_read(stm, &nrec, 4, &got);
    if (got != 4 || hr < 0) {
        r = (int32_t)0x80030002;
        goto out;
    }
    r = 0;
    for (uint32_t i = 0; i < nrec; i++) {
        uint32_t kl[2];
        hr = vc_stm_read(stm, kl, 8, &got);
        if (got != 8 || hr < 0) { r = (int32_t)0x80030002; break; }
        if (cap < kl[1]) {
            cap = kl[1];
            buf = vc_realloc(buf, kl[1]);
        }
        hr = vc_stm_read(stm, buf, kl[1], &got);
        if (kl[1] != got || hr < 0) { r = (int32_t)0x80030002; break; }
        uint8_t fl = (uint8_t)(kl[0] >> 24);
        if (fl & 0x80) continue;
        if (((fl & 0x40) && cfg_check_40(kl[0], kl[1], buf) != 1) || ((fl & 0x20) && cfg_check_20(kl[0], kl[1], buf) != 1) ||
            ((fl & 0x10) && cfg_check_10(kl[0], kl[1], buf) != 1)) {
            r = 0x32;
            break;
        }
    }
out:
    vc_com_release(stm);
    if (buf) vc_free(buf);
    if (idx == count) {
        vc_EnterCriticalSection(CS);
        vc_com_release(stg);
        vc_LeaveCriticalSection(CS);
        *stgp = 0;
    } else {
        GPSET(*stgp, stg);
    }
    return r;
}

int32_t mode_find(const ModeList *infos, const uint8_t *mode_id, uint32_t *idx) {
    uint32_t n = list_count(infos), i = 0;
    for (; i < n; i++) {
        const TtsModeInfo *m = GPN(const TtsModeInfo, list_item(infos, i));
        if (m && vc_memcmp(m->mode_id, mode_id, 16) == 0) break;
    }
    if (i >= n) return (int32_t)0x80040206;
    *idx = i;
    return 0;
}

// the highest rank seen per age class (features 0x200 / 0x100) and gender (2 / 1)
static void track_rank(int16_t best[4], const VoiceHdr *h) {
    int k;
    if (h->features & 0x200) k = (h->gender & 2) ? 0 : (h->gender & 1) ? 1 : -1;
    else if (h->features & 0x100) k = (h->gender & 2) ? 2 : (h->gender & 1) ? 3 : -1;
    else k = -1;
    if (k >= 0 && !(best[k] > h->_8f6)) best[k] = h->_8f6;
}

int32_t voices_enum(const char *dir, ModeList *paths, ModeList *hdrs, ModeList *infos) {
    char pat[0x100], path[0x104];
    uint8_t fd[0x140];
    static VoiceHdr hdr;            // (a stack frame in the original, kept across files)
    static VoiceCfg cfg;
    static TtsModeInfo info;
    GPTR(void) stg = 0;
    int16_t best[4] = { 0, 0, 0, 0 };
    snprintf(pat, sizeof pat, "%s\\*.vce", dir);
    uint32_t h = vc_FindFirstFileA(pat, fd);
    if (h == 0xffffffffu) return 0;
    do {
        snprintf(path, sizeof path, "%s\\%s", dir, (const char *)fd + 0x2c);
        int32_t r = voice_read_header(path, &hdr);
        track_rank(best, &hdr);
        if (r < 0) continue;
        mode_info(&hdr, &info);
        if (!list_add(paths, path, (uint32_t)strlen(path) + 1) || !list_add(infos, &info, 0xaf0) ||
            !list_add(hdrs, &hdr, 0x8f8)) {
            vc_FindClose(h);
            return (int32_t)0x8007000e;
        }
        int16_t neg = -1;
        for (int16_t i = 1; i <= hdr.variants; i++, neg--) {
            r = cfg_read(i, hdr.variants, path, &cfg, infos, &stg);
            cfg.h.variants = neg;
            track_rank(best, &cfg.h);
            if (r < 0) continue;
            mode_info_cfg(&cfg, &hdr, &info);
            if (!list_add(paths, path, (uint32_t)strlen(path) + 1) || !list_add(hdrs, &cfg, 0xb18) ||
                !list_add(infos, &info, 0xaf0)) {
                vc_FindClose(h);
                return (int32_t)0x8007000e;
            }
        }
    } while (vc_FindNextFileA(h, fd));
    vc_FindClose(h);
    snprintf(pat, sizeof pat, "%s\\*.cfg", dir);
    h = vc_FindFirstFileA(pat, fd);
    if (h == 0xffffffffu) return 0;
    do {
        snprintf(path, sizeof path, "%s\\%s", dir, (const char *)fd + 0x2c);
        if (voice_read_header(path, &hdr) < 0) continue;
        int16_t neg = -1;
        for (int16_t i = 1; i <= hdr.variants; i++, neg--) {
            if (cfg_read(i, hdr.variants, path, &cfg, infos, &stg) < 0) continue;
            uint32_t found;
            if (mode_find(infos, cfg.h.mode_id, &found) < 0) continue;
            track_rank(best, &cfg.h);
            VoiceHdr *base = GP(VoiceHdr, list_item(hdrs, found));
            cfg.h.variants = neg;
            mode_info_cfg(&cfg, &hdr, &info);
            if (!list_add(paths, path, (uint32_t)strlen(path) + 1) || !list_add(hdrs, &cfg, 0xb18) ||
                !list_add(infos, &info, 0xaf0)) {
                vc_FindClose(h);
                return (int32_t)0x8007000e;
            }
            base->variants++;
        }
    } while (vc_FindNextFileA(h, fd));
    vc_FindClose(h);
    uint32_t n = list_count(infos);
    uint32_t eax = 0;
    for (uint32_t i = 0; i < n; i++) {
        TtsModeInfo *m = GP(TtsModeInfo, list_item(infos, i));
        const VoiceHdr *v = GP(const VoiceHdr, list_item(hdrs, i));
        int k = -1;
        eax = GRAW(GHOST(v));
        if (v->features & 0x200) k = (v->gender & 2) ? 0 : (v->gender & 1) ? 1 : -1;
        else if (v->features & 0x100) k = (v->gender & 2) ? 2 : (v->gender & 1) ? 3 : -1;
        if (k < 0) continue;
        eax = (eax & 0xffff0000u) | (uint16_t)v->_8f6;
        if (v->_8f6 == best[k]) m->features |= 0x4000;
    }
#ifdef DECOMP_HOOK
    return (int32_t)eax;
#else
    return (int32_t)(eax & 0xffff);     // (the original's value carries a heap address's high half)
#endif
}

uint8_t *cfg_load(const char *path, int32_t idx) {
    void *stg = NULL, *stm = NULL;
    if (open_storage(path, &stg) < 0) return NULL;
    uint16_t name[20];
    wname(name, "CFG", idx);
    if (vc_stg_open_stream(stg, name, &stm) < 0) {
        vc_com_release(stg);
        return NULL;
    }
    uint32_t size = 0, got = 0;
    vc_stm_seek(stm, 0, 2, &size);
    vc_stm_seek(stm, 0, 0, NULL);
    uint8_t *buf = vc_malloc(size);
    int32_t hr = vc_stm_read(stm, buf, size, &got);
    vc_EnterCriticalSection(CS);
    vc_com_release(stg);
    vc_LeaveCriticalSection(CS);
    vc_com_release(stm);
    if (got != size || hr < 0) return NULL;
    return buf;
}

#define VT (*DLLPTR(GPTR(ModeTable), 0x63738bc0))
#define VT_CAP (*DLLVAR(int32_t, 0x63738bc4))
#define VT_N (*DLLVAR(int32_t, 0x63738bc8))

int32_t mode_table_add(const VoiceHdr *mode, const ModeList *paths, const ModeList *hdrs, int32_t *voice,
                       int32_t *variant, int32_t *index) {
    uint8_t id[16];
    memcpy(id, mode->mode_id, 16);
    *variant = 0;
    uint32_t n = list_count(paths), i = 0;
    const VoiceHdr *base = NULL;
    for (; i < n; i++) {
        base = GPN(const VoiceHdr, list_item(hdrs, i));
        if (base && base->variants >= 0 && vc_memcmp(id, base->mode_id, 16) == 0) break;
    }
    if (i == n) return (int32_t)0x80040206;
    *index = (int32_t)i;
    vc_EnterCriticalSection(CS);
    int32_t slot = -1;
    for (int32_t k = 0; k < VT_N; k++) {
        ModeTable *e = GPN(ModeTable, GP(GPTR(ModeTable), VT)[k]);
        if (e) {
            if (vc_memcmp(e, id, 16) == 0) {        // loaded already
                e->refs++;
                vc_LeaveCriticalSection(CS);
                *voice = k;
                if (!e->count) return 0;
                uint32_t n2 = list_count(paths);
                int32_t v = 1;
                for (uint32_t j = 0; j < n2; j++) {
                    const VoiceHdr *h = GPN(const VoiceHdr, list_item(hdrs, j));
                    if (!h || h->variants >= 0 || vc_memcmp(id, h->mode_id, 16) != 0) continue;
                    if (mode->variants < 0 &&
                        vc_memcmp(((const VoiceCfg *)(const void *)h)->mode_id, ((const VoiceCfg *)(const void *)mode)->mode_id, 16) == 0) {
                        *variant = -v;
                        return 0;
                    }
                    v++;
                }
                return 0;
            }
        } else if (slot == -1) {
            slot = k;
        }
    }
    if (slot == -1) slot = VT_N++;
    if (slot == VT_CAP) {
        VT_CAP += 10;
        GPSET(VT, (GPTR(ModeTable) *)vc_realloc(GPN(void, VT), (size_t)(uint32_t)VT_CAP * sizeof(GPTR(ModeTable))));
    }
    ModeTable *e = vc_calloc(1, sizeof(ModeTable));
    GPSET(GP(GPTR(ModeTable), VT)[slot], e);
    memcpy(e, id, 16);
    e->count = base->variants;
    if (base->variants) {
        GPTR(const uint8_t) *modes = vc_calloc((size_t)(uint32_t)(base->variants + 1), sizeof(GPTR(const uint8_t)));
        GPSET(e->modes, modes);
        uint32_t n2 = list_count(paths);
        int32_t neg = -1, at = 1;
        for (uint32_t j = 0; j < n2; j++) {
            const VoiceHdr *h = GPN(const VoiceHdr, list_item(hdrs, j));
            if (!h || h->variants >= 0 || vc_memcmp(id, h->mode_id, 16) != 0) continue;
            GPSET(modes[at], cfg_load(GP(const char, list_item(paths, j)), -h->variants));
            if (mode->variants < 0 &&
                vc_memcmp(((const VoiceCfg *)(const void *)h)->mode_id, ((const VoiceCfg *)(const void *)mode)->mode_id, 16) == 0)
                *variant = neg;
            at++;
            neg--;
        }
    }
    e->refs = 1;
    *voice = slot;
    vc_LeaveCriticalSection(CS);
    return 0;
}
