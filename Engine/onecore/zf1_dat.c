/* zf1_dat.c - MSTTSLocEnUS.dat container reader (see notes/frontend.md 1.1).  Portable C99. */
#include "zf1_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#define ZF1_DAT_MMAP 1   /* ClassicVoices patch: map the file read-only (clean, shared pages) instead of a malloc copy */
#endif

static void walk(zf1_dat *d, size_t o, size_t end, int depth)
{
    while (o + 0x28 <= end) {
        const uint8_t *h = d->buf + o;
        uint64_t sz = zf_rd64(h + 32);
        size_t c = o + 0x28;
        uint32_t t1 = zf_rd32(h);
        if (c + sz > end) return;
        if (t1 == 0x099f9814u || t1 == 0x3e13f66au || t1 == 0xe5f704bcu) {
            walk(d, c, c + (size_t)sz, depth + 1);
        } else if (depth >= 2 && d->nres < 64) {
            zf1_res *r = &d->res[d->nres++];
            memcpy(r->type, h, 16);
            memcpy(r->id, h + 16, 16);
            r->type1 = t1;
            r->id1 = zf_rd32(h + 16);
            r->p = d->buf + c;
            r->n = (size_t)sz;
        }
        o = (c + (size_t)sz + 7) & ~(size_t)7;
    }
}

int zf1_dat_load(zf1_dat *d, const char *path)
{
    FILE *f;
    long n;
    memset(d, 0, sizeof *d);
    if (!path) path = "C:\\Windows\\Speech_OneCore\\Engines\\TTS\\en-US\\MSTTSLocEnUS.dat";
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0x28) { fclose(f); return -2; }
#ifdef ZF1_DAT_MMAP
    {   /* ClassicVoices patch: the loader wants 16 zero bytes after the data; the kernel zero-fills the rest of the
           last page, so map when at least 16 bytes of it are left, else fall back to the copy below */
        long pg = sysconf(_SC_PAGESIZE);
        if (pg > 0 && n % pg != 0 && pg - n % pg >= 16) {
            void *m = mmap(NULL, (size_t)n, PROT_READ, MAP_PRIVATE, fileno(f), 0);
            if (m != MAP_FAILED) {
                fclose(f);
                d->buf = (uint8_t *)m;
                d->maplen = (size_t)n;
                d->size = (size_t)n;
                walk(d, 0, d->size, 0);
                return d->nres > 0 ? 0 : -5;
            }
        }
    }
#endif
    d->buf = (uint8_t *)malloc((size_t)n + 16);
    if (!d->buf) { fclose(f); return -3; }
    if (fread(d->buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d->buf); d->buf = NULL; return -4; }
    fclose(f);
    memset(d->buf + n, 0, 16);
    d->size = (size_t)n;
    walk(d, 0, d->size, 0);
    return d->nres > 0 ? 0 : -5;
}

void zf1_dat_free(zf1_dat *d)
{
#ifdef ZF1_DAT_MMAP
    if (d->maplen) munmap(d->buf, d->maplen);   /* ClassicVoices patch */
    else
#endif
    free(d->buf);
    memset(d, 0, sizeof *d);
}

const uint8_t *zf1_dat_get(const zf1_dat *d, uint32_t type1, uint32_t id1, size_t *size)
{
    int i;
    for (i = 0; i < d->nres; i++)
        if (d->res[i].type1 == type1 && d->res[i].id1 == id1) {
            if (size) *size = d->res[i].n;
            return d->res[i].p;
        }
    if (size) *size = 0;
    return NULL;
}

/* ClassicVoices patch: locale domain lists and a case-tolerant open (see zf1_int.h) */
static const zf1_domfile DOM_ENGB[] = {
    {"name", "enGB.Name.dat"}, {"message", "enGB.Message.dat"}, {"computer", "enGB.Computer.dat"},
    {"address", "EnGB.Address.dat"}, {"companyName", "EnGB.CompanyName.dat"}, {"cityName", "EnGB.CityName.dat"}};

/* 2026-09-29: en-AU, en-CA - each MSTTSLoc<xx>.INI [Domain] in its order, named as staged in OneCoreVoice */
static const zf1_domfile DOM_ENAU[] = {
    {"name", "EnAU.Name.dat"}, {"message", "EnAU.Message.dat"}, {"computer", "EnAU.Computer.dat"},
    {"address", "EnAU.Address.dat"}, {"companyName", "EnAU.CompanyName.dat"}, {"cityName", "EnAU.CityName.dat"}};
static const zf1_domfile DOM_ENCA[] = {
    {"address", "enCA.Address.dat"}, {"name", "enCA.Name.dat"}, {"message", "enCA.Message.dat"},
    {"computer", "enCA.Computer.dat"}, {"media", "enCA.Media.dat"}, {"companyName", "enCA.CompanyName.dat"}};
static const zf1_domfile DOM_ENIE[] = {
    {"address", "EnIE.Address.dat"}, {"name", "EnIE.Name.dat"}, {"message", "EnIE.Message.dat"},
    {"computer", "EnIE.Computer.dat"}, {"companyName", "EnIE.CompanyName.dat"}, {"cityName", "EnIE.CityName.dat"}};
static const zf1_domfile DOM_ENIN[] = {
    {"address", "EnIN.address.dat"}, {"name", "EnIN.Name.dat"}, {"message", "EnIN.message.dat"},
    {"computer", "EnIN.computer.dat"}};
static const zf1_domfile DOM_DEAT[] = {
    {"address", "DeAT.Address.dat"}, {"name", "DeAT.Name.dat"}, {"message", "DeAT.Message.dat"},
    {"computer", "DeAT.Computer.dat"}};
static const zf1_domfile DOM_DECH[] = {
    {"address", "DeCH.Address.dat"}, {"name", "DeCH.Name.dat"}, {"message", "DeCH.Message.dat"},
    {"computer", "DeCH.Computer.dat"}};
static const zf1_domfile DOM_DEDE[] = {
    {"address", "DeDE.Address.dat"}, {"name", "DeDE.Name.dat"}, {"message", "deDE.Message.dat"},
    {"computer", "DeDE.Computer.dat"}};
static const zf1_domfile DOM_ESES[] = {
    {"address", "EsES.Address.dat"}, {"name", "EsES.Name.dat"}, {"message", "esES.Message.dat"},
    {"computer", "EsES.Computer.dat"}};
static const zf1_domfile DOM_ESMX[] = {
    {"name", "EsMX.Name.dat"}, {"message", "EsMX.Message.dat"}, {"computer", "EsMX.Computer.dat"}};
static const zf1_domfile DOM_FRCA[] = {
    {"address", "FrCA.Address.dat"}, {"name", "FrCA.Name.dat"}, {"message", "FrCA.Message.dat"},
    {"computer", "FrCA.Computer.dat"}};
static const zf1_domfile DOM_FRFR[] = {
    {"address", "FrFR.Address.dat"}, {"name", "FrFR.Name.dat"}, {"message", "frFR.Message.dat"},
    {"computer", "FrFR.Computer.dat"}};
static const zf1_domfile DOM_FRCH[] = {
    {"address", "FrCH.Address.dat"}, {"name", "FrCH.Name.dat"}, {"message", "FrCH.Message.dat"},
    {"computer", "FrCH.Computer.dat"}};
static const zf1_domfile DOM_ITIT[] = {
    {"address", "ItIT.Address.dat"}, {"name", "ItIT.Name.dat"}, {"message", "itIT.Message.dat"},
    {"computer", "ItIT.Computer.dat"}};
static const zf1_domfile DOM_JAJP[] = {
    {"address", "JaJP.address.dat"}, {"name", "JaJP.name.dat"}, {"message", "JaJP.message.dat"},
    {"companyName", "JaJP.CompanyName.dat"}};
static const zf1_domfile DOM_PLPL[] = {
    {"address", "PlPL.Address.dat"}, {"name", "PlPL.Name.dat"}, {"message", "PlPL.Message.dat"},
    {"computer", "PlPL.Computer.dat"}};
static const zf1_domfile DOM_PTBR[] = {
    {"address", "PtBR.Address.dat"}, {"name", "PtBR.name.dat"}, {"message", "PtBR.message.dat"}};
static const zf1_domfile DOM_RURU[] = {
    {"address", "RuRU.Address.dat"}, {"name", "RuRU.Name.dat"}, {"message", "RuRU.Message.dat"},
    {"computer", "RuRU.Computer.dat"}};
static const zf1_domfile DOM_SVSE[] = {
    {"name", "SvSE.Name.dat"}};
static const zf1_domfile DOM_ZHCN[] = {
    {"address", "ZhCN.Address.dat"}, {"name", "ZhCN.Name.dat"}, {"message", "ZhCN.Message.dat"}};
static const zf1_domfile DOM_ZHHK[] = {
    {"address", "ZhHK.Address.dat"}, {"name", "ZhHK.Name.dat"}, {"message", "ZhHK.Message.dat"}};
#define DOMS(t) (*tab = t, (int)(sizeof t / sizeof *t))

int zf1_domain_files(int lcid, const zf1_domfile **tab)
{
    switch (lcid) {
    case 2057: return DOMS(DOM_ENGB);
    case 3081: return DOMS(DOM_ENAU);
    case 4105: return DOMS(DOM_ENCA);
    case 6153: return DOMS(DOM_ENIE);
    case 16393: return DOMS(DOM_ENIN);
    case 3079: return DOMS(DOM_DEAT);
    case 2055: return DOMS(DOM_DECH);
    case 1031: return DOMS(DOM_DEDE);
    case 3082: return DOMS(DOM_ESES);
    case 2058: return DOMS(DOM_ESMX);
    case 3084: return DOMS(DOM_FRCA);
    case 1036: return DOMS(DOM_FRFR);
    case 4108: return DOMS(DOM_FRCH);
    case 1040: return DOMS(DOM_ITIT);
    case 1041: return DOMS(DOM_JAJP);
    case 1045: return DOMS(DOM_PLPL);
    case 1046: return DOMS(DOM_PTBR);
    case 1049: return DOMS(DOM_RURU);
    case 1053: return DOMS(DOM_SVSE);
    case 2052: return DOMS(DOM_ZHCN);
    case 3076: return DOMS(DOM_ZHHK);
    }
    *tab = NULL;
    return 0;
}

int zf1_dat_load_ci(zf1_dat *d, char *path)
{
    size_t k = strlen(path);
    int r = zf1_dat_load(d, path);
    char c;
    if (r != -1) return r;
    while (k > 0 && path[k - 1] != '/' && path[k - 1] != '\\') k--;
    c = path[k];
    if (c >= 'a' && c <= 'z') path[k] = (char)(c - 32);
    else if (c >= 'A' && c <= 'Z') path[k] = (char)(c + 32);
    else return r;
    r = zf1_dat_load(d, path);
    path[k] = c;
    return r;
}
