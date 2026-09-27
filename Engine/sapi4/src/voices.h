// The engine's voices: the voice files (*.vce, compound files) and character files (*.cfg) of the
// engine's directory, the SAPI mode descriptions made from them, and the table of loaded voices the
// front end, unit stage and synthesizer look their data up in (voice.h's ModeTable). Layouts as in the
// original.
#pragma once
#include "gptr.h"
#include "voice.h"

// a growable list of malloc'd copies (C++ in the original)
typedef struct ListItem { GPTR(void) data; uint32_t size; } ListItem;
typedef struct ModeList {
    uint32_t count;             // 0x00
    uint32_t cap;               // 0x04 bytes allocated for items
    GPTR(ListItem) items;       // 0x08
} ModeList;
ModeList *list_ctor(ModeList *l);                       // @0x63674db7 thiscall
uint32_t list_count(const ModeList *l);                 // @0x63674e49 thiscall
int32_t list_grow(ModeList *l, uint32_t n);             // @0x63674df2 thiscall: room for n items; 1 = ok
// @0x63674e4c thiscall: append a copy of size bytes; 1 = ok, 0 if data or size is 0 or out of memory
int32_t list_add(ModeList *l, const void *data, uint32_t size);
GPTR(void) list_item(const ModeList *l, uint32_t i);    // @0x63674fcd thiscall: item i's data, 0 past the end

// a voice file's header (the VcHeader stream's 0x8f4 bytes, then two fields of the engine's own)
typedef struct VoiceHdr {
    uint8_t format[16];         // 0x000 file format id (three are accepted)
    uint16_t mfg[0x106];        // 0x010 manufacturer
    uint16_t product[0x106];    // 0x21c
    uint8_t mode_id[16];        // 0x428
    uint8_t language[0x82];     // 0x438 (a SAPI LANGUAGEW)
    uint16_t speaker[0x106];    // 0x4ba
    uint16_t style[0x106];      // 0x6c6
    uint16_t gender;            // 0x8d2 bit 0 female, bit 1 male
    uint16_t age;               // 0x8d4
    uint16_t rate;              // 0x8d6 default speaking rate
    uint16_t pitch;             // 0x8d8 base pitch
    uint16_t _8da;
    uint32_t features;          // 0x8dc SAPI TTSFEATURE_ bits (0x100, 0x200: the voice's age class)
    uint8_t _8e0[0x14];
    int16_t variants;           // 0x8f4 >= 0: a voice with this many characters; < 0: character -variants
    int16_t _8f6;               // 0x8f6 the voice's rank among voices of its kind (read from newer files)
} VoiceHdr;
// a character: a CFG stream's header (whose fields override the voice's where set), then records
typedef struct VoiceCfg {
    VoiceHdr h;                 // 0x000
    int32_t version;            // 0x8f8 1
    uint8_t mode_id[16];        // 0x8fc the character's own mode id
    uint16_t name[0x106];       // 0x90c
} VoiceCfg;
// SAPI 4 TTSMODEINFOW
typedef struct TtsModeInfo {
    uint8_t engine_id[16];      // 0x000
    uint16_t mfg[0x106];        // 0x010
    uint16_t product[0x106];    // 0x21c
    uint8_t mode_id[16];        // 0x428
    uint16_t mode_name[0x106];  // 0x438
    uint8_t language[0x82];     // 0x644
    uint16_t speaker[0x106];    // 0x6c6
    uint16_t style[0x106];      // 0x8d2
    uint16_t gender;            // 0xade
    uint16_t age;               // 0xae0
    uint16_t _ae2;
    uint32_t features;          // 0xae4
    uint32_t interfaces;        // 0xae8
    uint32_t engine_features;   // 0xaec
} TtsModeInfo;

// @0x6367d9ce stdcall: wide strcpy
void wstr_copy(uint16_t *dst, const uint16_t *src);
// @0x6367d9f4 stdcall: a voice file's header; 0, or STG_E_FILENOTFOUND (no file, no VcHeader, short
// or of an unknown format)
int32_t voice_read_header(const char *path, VoiceHdr *hdr);
// @0x6367db68 stdcall: the mode description of a voice header
void mode_info(const VoiceHdr *h, TtsModeInfo *out);
// @0x6367dc4d stdcall: the mode description of a character: its header merged over the voice's
// (fields it leaves empty come from the voice), with the character's own mode id; cfg->h becomes the
// merged header (format and mode id kept)
void mode_info_cfg(VoiceCfg *cfg, const VoiceHdr *voice, TtsModeInfo *out);
// @0x6367de41, @0x6367de55, @0x6367de5a stdcall: the checks of a character record with flag 0x40, 0x20
// or 0x10 in its key's top byte (1 = accepted)
int32_t cfg_check_40(uint32_t key, uint32_t len, const void *data);
int32_t cfg_check_20(uint32_t key, uint32_t len, const void *data);
int32_t cfg_check_10(uint32_t key, uint32_t len, const void *data);
// @0x6367de79 stdcall: character idx (1..count) of a voice file: its header into *cfg and its records
// checked; the storage stays open in *stg between calls and is closed after the last. 0, 0x32 (a
// record fails its check), or an error.
int32_t cfg_read(int16_t idx, int16_t count, const char *path, VoiceCfg *cfg, const ModeList *infos, GPTR(void) *stg);
// @0x6367e71b stdcall: the index of the mode with this id (TTSMODEINFO list); 0 or TTSERR_INVALIDMODE
int32_t mode_find(const ModeList *infos, const uint8_t *mode_id, uint32_t *idx);
// @0x6367e0ee stdcall: all voices and characters of a directory: *.vce files, their CFG streams,
// then *.cfg files (characters of voices already found); paths, headers and mode descriptions appended
// to the three lists; the first voice of each age class and gender marked (0x4000). Returns what the
// last comparison left in EAX (a non-negative value; 0 if there are no voices), or E_OUTOFMEMORY.
int32_t voices_enum(const char *dir, ModeList *paths, ModeList *hdrs, ModeList *infos);
// @0x6367efa0 stdcall: the CFG%i stream of a voice file, whole (malloc'd); 0 on failure
uint8_t *cfg_load(const char *path, int32_t idx);
// @0x6367f0c9 stdcall: find or add the voice of mode `mode` in the voice table (with its characters'
// CFG data); *voice its index, *variant the character (0, or -index), *index the voice's position in
// the lists. 0, or TTSERR_INVALIDMODE.
int32_t mode_table_add(const VoiceHdr *mode, const ModeList *paths, const ModeList *hdrs, int32_t *voice,
                       int32_t *variant, int32_t *index);
