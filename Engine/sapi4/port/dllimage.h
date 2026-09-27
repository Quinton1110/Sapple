// The portable build's data layer: msttssyn.dll's sections as the decompiled code addresses them
// (DLLVAR / DLLPTR with the DLL's preferred-base addresses). Two sources, same result:
//  - at run time from the user's copy of the DLL (dll_image_load), or
//  - from a build-time generated image compiled into the program (dll_image_use with the DllBlob that
//    port/mkimage writes: voice-baked builds, like iTruVoice's tvdata.s). The generated file contains
//    Microsoft's data and is never committed.
#pragma once
#include <stdint.h>

typedef struct DllBlobSection {
    uint32_t va;                // offset in the image
    uint32_t size;              // bytes present (the rest of the section is zero)
    const uint8_t *data;
} DllBlobSection;
typedef struct DllBlob {
    uint32_t image_size;
    uint32_t nsections;
    const DllBlobSection *sections;
    uint32_t nslots;            // pointer-holding dwords: image offsets (from the relocations)
    const uint32_t *slots;
} DllBlob;

// load msttssyn.dll's sections from `path` (1: ok)
int dll_image_load(const char *path);
// use a compiled-in image (1: ok)
int dll_image_use(const DllBlob *blob);
// the image as a blob (after dll_image_load: for the generator)
DllBlob dll_image_blob(void);
void *decomp_dll_data(uint32_t addr);
void **decomp_dll_ptr(uint32_t addr);
