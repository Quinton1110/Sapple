/* zb_io.c - golden-file loaders and WAV writer.
 *
 * zbtap log (harness/zbtap.exe): sequence of records {char tag[4]; u32 nbytes; u8 payload[nbytes]}:
 *   'HDR '  i32 version(1), i32 siteRate, i32 siteVolume, i32 0, then the token id as UTF-16LE
 *   'UTT '  i32 index, i32 -1                                       one per sentence (backend entry)
 *   'FEAT'  u32 nfeat, u32 nphone, i32 cell[nfeat][nphone][2] = {kind, value}; kind -1 = null (value -1),
 *           kind 0/1 = int/enum, kind 2 = string (value indexes the following 'FSTR'); columns in APM order
 *   'FSTR'  u32 n, n x {u32 nchars, u16 chars[]}
 *   'PHON'  u32 nphone, nphone x 92-byte ZbTapPhone {u16 id, u16 pad, i32 isPauseLike, i32 isSil, i32 wordIndex,
 *           i32 wordType, i32 word90, i32 skip, i32 prosody[16] (phone+0x78 block)}
 *   'DUR '  u32 rows, cols, u32 d[]                (state durations; rows = phones, cols = 5)
 *   'VUV '  u32 0, u32 T, i32 v[T]
 *   'LF0 ' 'LSF ' 'GAIN'  u32 rows, cols, f32 (float path) or i32 (int path) data after each stream predictor
 *   'INTF'  i32 0, i32 intFlag                     (at CWaveGenerator::Begin)
 *   'FIN0' 'FIN1' 'FIN2'  final F0 [Hz] / LSF / gain matrices handed to the vocoder
 *   'PCM '  i16 samples[]                          (engine output writes)
 *   'UEND'                                         (end-of-utterance vocoder reset)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zb_internal.h"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void default_ctl(ZbPhoneCtl *c)
{
    memset(c, 0, sizeof *c);
    c->volume = 100;
    c->word = -1;
    c->word_type = -1;
}

static void *dupmat(const uint8_t *b, uint32_t n, int *rows, int *cols)
{
    uint32_t r = rd32(b), c = rd32(b + 4);
    void *d;
    if (8 + (size_t)r * c * 4 > n) return NULL;
    d = malloc((size_t)r * c * 4 + 4);
    memcpy(d, b + 8, (size_t)r * c * 4);
    if (rows) *rows = (int)r;
    if (cols) *cols = (int)c;
    return d;
}

int zb_load_zbtap(const char *path, const ZbVoice *v, ZbGolden *g)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long sz, o = 0;
    int cur = -1, cap = 0;
    memset(g, 0, sizeof *g);
    g->sapi.volume = 100;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc((size_t)sz + 1);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); return -1; }
    fclose(f);
    while (o + 8 <= sz) {
        const char *tag = (const char *)d + o;
        uint32_t n = rd32(d + o + 4);
        const uint8_t *b = d + o + 8;
        if (o + 8 + (long)n > sz) break;
        if (!memcmp(tag, "HDR ", 4) && n >= 16) {
            uint32_t i;
            g->sapi.rate = (int)rd32(b + 4);
            g->sapi.volume = (int)rd32(b + 8);
            for (i = 0; i < (n - 16) / 2 && i < sizeof g->token - 1; i++) g->token[i] = (char)b[16 + 2 * i];
            g->token[i] = 0;
        } else if (!memcmp(tag, "UTT ", 4)) {
            if (g->nutt == cap) {
                cap = cap ? cap * 2 : 8;
                g->utt = (ZbUtt *)realloc(g->utt, sizeof(ZbUtt) * cap);
                g->ref = (ZbTrace *)realloc(g->ref, sizeof(ZbTrace) * cap);
                g->pcm = (short **)realloc(g->pcm, sizeof(short *) * cap);
                g->npcm = (long *)realloc(g->npcm, sizeof(long) * cap);
            }
            cur = g->nutt++;
            memset(&g->utt[cur], 0, sizeof(ZbUtt));
            memset(&g->ref[cur], 0, sizeof(ZbTrace));
            g->pcm[cur] = NULL;
            g->npcm[cur] = 0;
        } else if (cur >= 0 && !memcmp(tag, "FEAT", 4)) {
            uint32_t nf = rd32(b), np = rd32(b + 4), i, p;
            ZbUtt *u = &g->utt[cur];
            if (nf != (uint32_t)v->nfeat) { fprintf(stderr, "zbtap: %u features, voice has %d\n", nf, v->nfeat); free(d); return -1; }
            u->nphone = (int)np;
            u->ph = (ZbPhone *)calloc(np, sizeof(ZbPhone));
            for (p = 0; p < np; p++) {
                int k;
                for (k = 0; k < ZB_NFEAT; k++) u->ph[p].f[k] = ZB_NULL;
                default_ctl(&u->ph[p].c);
            }
            for (i = 0; i < nf; i++)
                for (p = 0; p < np; p++) {
                    int kind = (int)rd32(b + 8 + 8 * ((size_t)i * np + p)), val = (int)rd32(b + 12 + 8 * ((size_t)i * np + p));
                    u->ph[p].f[v->featcol[i]] = kind == -1 ? ZB_NULL : val;
                }
        } else if (cur >= 0 && !memcmp(tag, "PHON", 4)) {
            uint32_t np = rd32(b), p;
            ZbUtt *u = &g->utt[cur];
            for (p = 0; p < np && (int)p < u->nphone; p++) {
                const uint8_t *e = b + 4 + 92 * p, *ctl = e + 28;
                ZbPhoneCtl *c = &u->ph[p].c;
                uint32_t fr = rd32(ctl + 12);
                c->is_pause = (int)rd32(e + 4);
                c->is_sil = (int)rd32(e + 8);
                c->word = (int)rd32(e + 12);
                c->word_type = (int)rd32(e + 16);
                c->volume = (int)rd32(ctl);
                c->rate = (int)rd32(ctl + 4);
                c->pitch = (int)rd32(ctl + 8);
                memcpy(&c->pitch_range, &fr, 4);
                c->emph = (int)rd32(ctl + 16);
                c->group_ms = (int)rd32(ctl + 20);
                c->phone_ms = (int)rd32(ctl + 24);
            }
        } else if (cur >= 0 && !memcmp(tag, "DUR ", 4)) {
            g->ref[cur].dur = (int *)dupmat(b, n, &g->ref[cur].nphone, &g->ref[cur].nstate);
        } else if (cur >= 0 && !memcmp(tag, "VUV ", 4)) {
            g->ref[cur].vuv = (int *)dupmat(b, n, NULL, &g->ref[cur].nframe);
        } else if (cur >= 0 && !memcmp(tag, "PHX ", 4)) {
            uint32_t np = rd32(b), p;
            for (p = 0; p < np && (int)p < g->utt[cur].nphone && 4 + 12 * p + 12 <= n; p++) {
                int x = (int)rd32(b + 4 + 12 * p + 4);
                g->utt[cur].ph[p].c.seg_rate = x == -99 ? 0 : x;
            }
        } else if (cur >= 0 && !memcmp(tag, "PDOM", 4)) {
            uint32_t np = rd32(b), p, o2 = 4;
            for (p = 0; p < np && (int)p < g->utt[cur].nphone && o2 + 8 <= n; p++) {
                int w90 = (int)rd32(b + o2), len = (int)rd32(b + o2 + 4), k;
                ZbPhoneCtl *c = &g->utt[cur].ph[p].c;
                o2 += 8;
                c->word90 = w90;
                for (k = 0; k < len && k < 31; k++) c->domain[k] = (char)b[o2 + 2 * k];
                c->domain[k < 31 ? k : 31] = 0;
                if (!strcmp(c->domain, "none")) c->domain[0] = 0;
                o2 += 2 * (uint32_t)len;
            }
        } else if (cur >= 0 && !memcmp(tag, "QIA2", 4) && n >= 8) {
            /* the engine's question-intonation anchor query succeeded: the final word node has sentence type 3 */
            if (rd32(b) == 0 && g->utt[cur].nphone > 0) g->utt[cur].ph[g->utt[cur].nphone - 1].c.sent_type = 3;
        } else if (cur >= 0 && !memcmp(tag, "LSFC", 4) && v->isint && g->ref[cur].dur) {
            /* int path: LSF/gain chunk snapshot right after generation: copy the rows of phones [start, start+count) */
            ZbTrace *r = &g->ref[cur];
            uint32_t start = rd32(b), cnt = rd32(b + 4), lr = rd32(b + 12), lc = rd32(b + 16), p, f0 = 0, f1 = 0, s;
            if (!r->ilsf) { r->ilsf = (int *)calloc((size_t)lr * lc + 1, sizeof(int)); r->igain = (int *)calloc((size_t)lr + 1, sizeof(int)); r->order = (int)lc; }
            for (p = 0; p < start + cnt && (int)p < r->nphone; p++)
                for (s = 0; s < (uint32_t)r->nstate; s++) { if (p < start) f0 += (uint32_t)r->dur[p * r->nstate + s]; f1 += (uint32_t)r->dur[p * r->nstate + s]; }
            if (f1 <= lr && 28 + ((size_t)lr * lc + lr) * 4 <= n) {
                memcpy(r->ilsf + (size_t)f0 * lc, b + 28 + (size_t)f0 * lc * 4, (size_t)(f1 - f0) * lc * 4);
                memcpy(r->igain + f0, b + 28 + (size_t)lr * lc * 4 + (size_t)f0 * 4, (size_t)(f1 - f0) * 4);
            }
        } else if (cur >= 0 && !memcmp(tag, "LF0 ", 4)) {
            if (v->isint) g->ref[cur].ilf0 = (int *)dupmat(b, n, NULL, NULL);
            else g->ref[cur].lf0 = (float *)dupmat(b, n, NULL, NULL);
        } else if (cur >= 0 && !memcmp(tag, "LSF ", 4)) {
            g->ref[cur].lsf = (float *)dupmat(b, n, NULL, &g->ref[cur].order);
        } else if (cur >= 0 && !memcmp(tag, "GAIN", 4)) {
            g->ref[cur].gain = (float *)dupmat(b, n, NULL, NULL);
        } else if (cur >= 0 && !memcmp(tag, "FIN0", 4)) {
            if (v->isint) g->ref[cur].if0 = (int *)dupmat(b, n, NULL, NULL);
            else g->ref[cur].f0 = (float *)dupmat(b, n, NULL, NULL);
        } else if (cur >= 0 && !memcmp(tag, "FIN1", 4) && !v->isint) {
            g->ref[cur].lsf_fin = (float *)dupmat(b, n, NULL, NULL);
        } else if (cur >= 0 && !memcmp(tag, "PCM ", 4)) {
            long m = (long)(n / 2);
            g->pcm[cur] = (short *)realloc(g->pcm[cur], sizeof(short) * (size_t)(g->npcm[cur] + m + 1));
            memcpy(g->pcm[cur] + g->npcm[cur], b, (size_t)m * 2);
            g->npcm[cur] += m;
        }
        o += 8 + (long)n;
    }
    free(d);
    return 0;
}

/* ziraprobe.exe text output: "FeatureExtract n=N ..." followed by N lines "  fII cnt=C: v v v", v = "null" |
 * "kind:value" | 'string'. Only the backend matrix (n > 1) is read; per-phone attributes get plain-text defaults
 * (silence phones: id 3; leading 100 ms, trailing = [SilenceLength] SentenceBoundary). */
int zb_load_ziraprobe(const char *path, const ZbVoice *v, ZbGolden *g)
{
    FILE *f = fopen(path, "r");
    char *line;
    int n = 0, row = 0, cap = 0;
    ZbUtt *u = NULL;
    memset(g, 0, sizeof *g);
    g->sapi.volume = 100;
    if (!f) return -1;
    line = (char *)malloc(1 << 20);
    while (fgets(line, 1 << 20, f)) {
        int fi, cnt;
        if (sscanf(line, "FeatureExtract n=%d", &n) == 1) {
            row = 0;
            if (n <= 1) { n = 0; continue; }
            if (g->nutt == cap) {
                cap = cap ? cap * 2 : 8;
                g->utt = (ZbUtt *)realloc(g->utt, sizeof(ZbUtt) * cap);
            }
            u = &g->utt[g->nutt++];
            memset(u, 0, sizeof *u);
            continue;
        }
        if (n > 1 && u && sscanf(line, " f%d cnt=%d:", &fi, &cnt) == 2 && fi == row && fi < v->nfeat) {
            char *p = strchr(line, ':') + 1;
            int k;
            if (!u->ph) {
                u->nphone = cnt;
                u->ph = (ZbPhone *)calloc((size_t)cnt, sizeof(ZbPhone));
                for (k = 0; k < cnt; k++) {
                    int j;
                    for (j = 0; j < ZB_NFEAT; j++) u->ph[k].f[j] = ZB_NULL;
                    default_ctl(&u->ph[k].c);
                }
            }
            for (k = 0; k < cnt && k < u->nphone; k++) {
                int kind, val;
                while (*p == ' ') p++;
                if (!strncmp(p, "null", 4)) { val = ZB_NULL; p += 4; }
                else if (sscanf(p, "%d:%d", &kind, &val) == 2) { while (*p && *p != ' ' && *p != '\n') p++; }
                else break;
                u->ph[k].f[v->featcol[fi]] = val;
            }
            row++;
            if (row == n) {
                for (k = 0; k < u->nphone; k++) {
                    ZbPhoneCtl *c = &u->ph[k].c;
                    c->is_sil = u->ph[k].f[ZB_F_PhoneIdentity] == 3;
                    c->word = k;
                }
                if (u->nphone) {
                    if (u->ph[0].c.is_sil) u->ph[0].c.phone_ms = 100;
                    if (u->ph[u->nphone - 1].c.is_sil) u->ph[u->nphone - 1].c.phone_ms = v->cfg.sentence_boundary_ms;
                }
                n = 0;
            }
        }
    }
    free(line);
    fclose(f);
    return 0;
}

void zb_golden_free(ZbGolden *g)
{
    int i;
    for (i = 0; i < g->nutt; i++) {
        free(g->utt[i].ph);
        if (g->ref) zb_trace_free(&g->ref[i]);
        if (g->pcm) free(g->pcm[i]);
    }
    free(g->utt);
    free(g->ref);
    free(g->pcm);
    free(g->npcm);
    memset(g, 0, sizeof *g);
}

int zb_write_wav(const char *path, const short *pcm, long n)
{
    FILE *f = fopen(path, "wb");
    uint8_t h[44];
    uint32_t data = (uint32_t)n * 2, x;
    long i;
    if (!f) return -1;
#define W32(o, v) (x = (v), h[o] = (uint8_t)x, h[o + 1] = (uint8_t)(x >> 8), h[o + 2] = (uint8_t)(x >> 16), h[o + 3] = (uint8_t)(x >> 24))
    memcpy(h, "RIFF", 4); W32(4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8); W32(16, 16);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    W32(24, 16000); W32(28, 32000);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); W32(40, data);
#undef W32
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) { uint8_t s[2]; s[0] = (uint8_t)(pcm[i] & 0xff); s[1] = (uint8_t)((uint16_t)pcm[i] >> 8); fwrite(s, 1, 2, f); }
    fclose(f);
    return 0;
}
