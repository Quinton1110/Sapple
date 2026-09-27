// A small read-only reader of compound files (MS-CFB v3/v4, OLE structured storage): the voice
// files are such files. Used by the emulator's StgOpenStorage and by the portable build's storage.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct CfbEntry {
    char name[64];      // UTF-16 name folded to Latin-1
    int type;           // 1 storage, 2 stream, 5 root
    uint32_t left, right, child, start;
    uint64_t size;
} CfbEntry;

typedef struct Cfb {
    const uint8_t *data; size_t len;    // the whole file (mapped read-only, or the caller's memory)
    int mapped;
    uint32_t ssz, mssz, cutoff;
    uint32_t *fat; uint32_t nfat;
    uint32_t *mfat; uint32_t nmfat;
    CfbEntry *ents; uint32_t nents;
    uint8_t *ministream; size_t ministream_len;
    int refs;
} Cfb;

Cfb *cfb_open(const char *path);                            // NULL if not a compound file
Cfb *cfb_open_mem(const uint8_t *data, size_t len);         // (data stays the caller's)
void cfb_release(Cfb *f);                                   // reference counted (refs starts at 1)
uint8_t *cfb_stream(Cfb *f, const CfbEntry *e, size_t *len);    // a stream's bytes (malloc)
// a child of storage entry idx by (case-insensitive) name: the sibling tree under its child pointer is
// searched by the caller passing ents[idx].child; -1 if absent
int cfb_find(Cfb *f, uint32_t idx, const char *name);
