// Compound file reader (see cfb.h), moved out of the emulator so that the portable build uses the same.
#include "cfb.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENDOFCHAIN 0xFFFFFFFAu
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint8_t *cfb_sector(Cfb *f, uint32_t s) {
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
        const uint8_t *sec = cfb_sector(f, s);
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

static Cfb *cfb_parse(Cfb *f);

Cfb *cfb_open(const char *path) {
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
    f->mapped = 1;
    return cfb_parse(f);
}

Cfb *cfb_open_mem(const uint8_t *data, size_t len) {
    Cfb *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->data = data;
    f->len = len;
    return cfb_parse(f);
}

static Cfb *cfb_parse(Cfb *f) {
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
        const uint8_t *sec = cfb_sector(f, s);
        if (!sec) break;
        for (uint32_t i = 0; i + 1 < f->ssz / 4; i++) difat[nd++] = le32(sec + 4 * i);
        s = le32(sec + f->ssz - 4);
    }
    f->nfat = nfatsec * (f->ssz / 4);
    f->fat = malloc(sizeof(uint32_t) * f->nfat);
    for (uint32_t i = 0; i < nfatsec; i++) {
        const uint8_t *sec = cfb_sector(f, difat[i]);
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
    if (f->mapped) munmap((void *)f->data, f->len);
    free(f->fat); free(f);
    return NULL;
}
void cfb_release(Cfb *f) {
    if (--f->refs > 0) return;
    if (f->mapped) munmap((void *)f->data, f->len); free(f->fat); free(f->mfat); free(f->ents); free(f->ministream); free(f);
}
uint8_t *cfb_stream(Cfb *f, const CfbEntry *e, size_t *len) {
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
int cfb_find(Cfb *f, uint32_t idx, const char *name) {
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

