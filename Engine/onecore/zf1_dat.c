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
#define DOMS(t) (*tab = t, (int)(sizeof t / sizeof *t))

int zf1_domain_files(int lcid, const zf1_domfile **tab)
{
    if (lcid == 2057) { *tab = DOM_ENGB; return (int)(sizeof DOM_ENGB / sizeof *DOM_ENGB); }
    if (lcid == 3081) return DOMS(DOM_ENAU);
    if (lcid == 4105) return DOMS(DOM_ENCA);
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
