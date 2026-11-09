/* IBM ProTALKER 97 Japanese TTS engine (読み秀君 / 読み子ちゃん) running in the SAPI4/emu x86 interpreter. */
#include "protalker_tts.h"
#include "emu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <dlfcn.h>
typedef const void *PT_CFStringRef;
typedef long PT_CFIndex;
typedef uint32_t PT_CFStringEncoding;
typedef struct { PT_CFIndex location; PT_CFIndex length; } PT_CFRange;
#define PT_kCFStringEncodingUTF8 0x08000100u
#define PT_kCFStringEncodingDOSJapanese 0x0420u
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define PROTALKER_MEM_SIZE 0x08000000u /* 128 MB guest space */

static char s_bundle_dir[1024] = {0};
static char s_extra_dir[1024] = {0};

void protalker_tts_set_bundle_dir(const char *dir)
{
    if (dir && *dir) {
        snprintf(s_bundle_dir, sizeof s_bundle_dir, "%s", dir);
    } else {
        s_bundle_dir[0] = '\0';
    }
}

void protalker_tts_set_extra_dir(const char *dir)
{
    if (dir && *dir) {
        snprintf(s_extra_dir, sizeof s_extra_dir, "%s", dir);
    } else {
        s_extra_dir[0] = '\0';
    }
}

int protalker_tts_is_voice(const char *voice_name)
{
    if (!voice_name) return 0;
    return strcmp(voice_name, "ProTalker_Yomihide") == 0 ||
           strcmp(voice_name, "ProTalker_Hide") == 0 ||
           strcmp(voice_name, "読み秀君") == 0 ||
           strcmp(voice_name, "ProTalker_Yomiko") == 0 ||
           strcmp(voice_name, "ProTalker_Ko") == 0 ||
           strcmp(voice_name, "読み子ちゃん") == 0;
}

struct protalker_tts {
    Emu *e;
    EmuModule *mod;
    int actor; /* 1 = 読み秀君 (WFCM1000), 2 = 読み子ちゃん (WFCF1000) */
    uint32_t fn_init, fn_cntl, fn_speak, fn_term;
    uint32_t g_err, g_handle, g_user_str, g_cntl;
    uint32_t handle_val;
    uint32_t cb_thunk;
    int cur_speed, cur_pitch;

    protalker_audio_cb cur_cb;
    void *cur_user;
    int stopped;
    int16_t zero_buf[2048];
};

static void on_guest_pcm(Emu *e, X86 *c)
{
    protalker_tts *t = (protalker_tts *)emu_user(e);
    uint32_t pcm_ptr = x86_arg(c, 0);
    uint32_t byte_len = x86_arg(c, 1);
    if (!t || t->stopped || !t->cur_cb) {
        c->s.r[EAX] = 0;
        return;
    }
    size_t samples = byte_len / 2;
    if (samples == 0) {
        c->s.r[EAX] = byte_len;
        return;
    }
    if (pcm_ptr != 0) {
        const int16_t *pcm = (const int16_t *)emu_ptr(e, pcm_ptr, byte_len);
        if (t->cur_cb(pcm, samples, t->cur_user)) {
            t->stopped = 1;
            c->s.r[EAX] = 0;
            return;
        }
    } else {
        size_t rem = samples;
        while (rem > 0) {
            size_t chunk = rem > 2048 ? 2048 : rem;
            if (t->cur_cb(t->zero_buf, chunk, t->cur_user)) {
                t->stopped = 1;
                c->s.r[EAX] = 0;
                return;
            }
            rem -= chunk;
        }
    }
    c->s.r[EAX] = byte_len;
}

static int file_exists_ci(const char *dir, const char *sub, char *out_dir, size_t out_sz, char *out_dll, size_t dll_sz)
{
    if (!dir || !*dir) return 0;
    if (sub && *sub) {
        snprintf(out_dir, out_sz, "%s/%s", dir, sub);
    } else {
        snprintf(out_dir, out_sz, "%s", dir);
    }
    static const char *const dll_names[] = { "Tts_mgr.dll", "TTS_MGR.DLL", "tts_mgr.dll" };
    struct stat st;
    for (int i = 0; i < 3; i++) {
        snprintf(out_dll, dll_sz, "%s/%s", out_dir, dll_names[i]);
        if (stat(out_dll, &st) == 0 && st.st_size > 0) return 1;
    }
    return 0;
}

static int resolve_protalker_dir(const char *dir, char *out_dir, size_t out_sz, char *out_dll, size_t dll_sz)
{
    if (s_bundle_dir[0]) {
        if (file_exists_ci(s_bundle_dir, NULL, out_dir, out_sz, out_dll, dll_sz)) return 1;
        if (file_exists_ci(s_bundle_dir, "../ProTalkerVoice", out_dir, out_sz, out_dll, dll_sz)) return 1;
    }
    if (file_exists_ci(dir, NULL, out_dir, out_sz, out_dll, dll_sz)) return 1;
    if (file_exists_ci(dir, "ProTalkerVoice", out_dir, out_sz, out_dll, dll_sz)) return 1;
    if (file_exists_ci(dir, "../ProTalkerVoice", out_dir, out_sz, out_dll, dll_sz)) return 1;
    if (s_extra_dir[0]) {
        if (file_exists_ci(s_extra_dir, NULL, out_dir, out_sz, out_dll, dll_sz)) return 1;
        if (file_exists_ci(s_extra_dir, "../ProTalkerVoice", out_dir, out_sz, out_dll, dll_sz)) return 1;
    }
    return 0;
}

/* spcSynCntl parameter layout (verified from Ptengine.dll 0x1000244d & Tts_mgr.dll 0x1001a647):
 *   +0x00: speed     (4..16, default 9; corresponds to SAPI4 dwSpeed 80..260, default 180)
 *   +0x04: pitch     (50..170, default 80 for BOTH 読み秀君 and 読み子ちゃん)
 *   +0x08: volume    (0..9, default 9 = 100% synthesis level)
 *   +0x0C: accent    (0..9, default 5 = standard intonation strength)
 *   +0x10: actor     (1 = 読み秀君 WFCM1000, 2 = 読み子ちゃん WFCF1000)
 *   +0x14: fixed     (1)
 *   +0x18: translate (0x004D = 0x0C | 0x41 in Ptengine.dll: English word & digit-place conversion)
 */
static void apply_cntl(protalker_tts *t, int speed, int pitch, int volume, int accent, uint16_t mask)
{
    X86 *c = emu_cpu(t->e);
    wr32(c, t->g_cntl + 0x00, (uint32_t)speed);
    wr32(c, t->g_cntl + 0x04, (uint32_t)pitch);
    wr32(c, t->g_cntl + 0x08, (uint32_t)volume);
    wr32(c, t->g_cntl + 0x0C, (uint32_t)accent);
    wr32(c, t->g_cntl + 0x10, (uint32_t)t->actor);
    wr32(c, t->g_cntl + 0x14, 1u);
    wr16(c, t->g_cntl + 0x18, mask);
    uint32_t args[2] = { t->handle_val, t->g_cntl };
    emu_call(t->e, t->fn_cntl, 2, args);
    t->cur_speed = speed;
    t->cur_pitch = pitch;
}

protalker_tts *protalker_tts_open(const char *dir, const char *voice_name, char *err, size_t errlen)
{
    char pt_dir[1100], dll_path[1200];
    if (!protalker_tts_is_voice(voice_name)) {
        if (err && errlen) snprintf(err, errlen, "unknown ProTalker voice");
        return NULL;
    }
    if (!resolve_protalker_dir(dir, pt_dir, sizeof pt_dir, dll_path, sizeof dll_path)) {
        if (err && errlen) snprintf(err, errlen, "ProTalkerVoice/Tts_mgr.dll not found");
        return NULL;
    }
    protalker_tts *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->actor = (strcmp(voice_name, "ProTalker_Yomiko") == 0 ||
                strcmp(voice_name, "ProTalker_Ko") == 0 ||
                strcmp(voice_name, "読み子ちゃん") == 0) ? 2 : 1;
    t->e = emu_create(PROTALKER_MEM_SIZE);
    if (!t->e) {
        if (err && errlen) snprintf(err, errlen, "emu_create failed");
        free(t);
        return NULL;
    }
    emu_set_user(t->e, t);
    emu_map_dir(t->e, "C:\\PROTALK", pt_dir);
    t->mod = emu_load_pe(t->e, dll_path, "C:\\PROTALK\\Tts_mgr.dll", EMU_IMAGE_BASE);
    if (!t->mod || !emu_dll_attach(t->e, t->mod)) {
        if (err && errlen) snprintf(err, errlen, "failed to load Tts_mgr.dll: %s", emu_error(t->e));
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    t->fn_init = emu_get_export(t->e, t->mod, "spcSynInit");
    t->fn_cntl = emu_get_export(t->e, t->mod, "spcSynCntl");
    t->fn_speak = emu_get_export(t->e, t->mod, "spcSynSpeak");
    t->fn_term = emu_get_export(t->e, t->mod, "spcSynTerm");
    if (!t->fn_init || !t->fn_cntl || !t->fn_speak || !t->fn_term) {
        if (err && errlen) snprintf(err, errlen, "missing spcSyn* exports in Tts_mgr.dll");
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    t->g_err = emu_static(t->e, 16);
    t->g_handle = emu_static(t->e, 16);
    t->g_user_str = emu_strdup(t->e, "USER");
    t->g_cntl = emu_static(t->e, 64);
    t->cb_thunk = emu_thunk(t->e, "protalker_pcm_cb", on_guest_pcm, 8);

    uint32_t init_args[4] = { 0x0000F001u, t->g_err, t->g_handle, t->g_user_str };
    uint32_t rc = emu_call(t->e, t->fn_init, 4, init_args);
    t->handle_val = rd32(emu_cpu(t->e), t->g_handle);
    if (emu_failed(t->e) || rc != 0 || !t->handle_val) {
        if (err && errlen) snprintf(err, errlen, "spcSynInit failed (rc=%u, err=%u)", rc, rd32(emu_cpu(t->e), t->g_err));
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    /* Match user's NVDA ProTalker settings (rate=25 -> dwSpeed=120 -> speed=6, pitch=55.5 -> wPitch=97, volume=9, accent=5, mask=0x004D) */
    apply_cntl(t, 6, 97, 9, 5, 0x004Du);
    return t;
}

void protalker_tts_close(protalker_tts *t)
{
    if (!t) return;
    if (t->e && t->fn_term && t->handle_val && !emu_failed(t->e)) {
        uint32_t args[1] = { t->handle_val };
        emu_call(t->e, t->fn_term, 1, args);
    }
    if (t->e) emu_destroy(t->e);
    free(t);
}

static char *utf8_to_sjis_alloc(const char *utf8)
{
    if (!utf8 || !*utf8) return strdup("");
#ifdef __APPLE__
    typedef PT_CFStringRef (*fn_create)(const void *, const char *, PT_CFStringEncoding);
    typedef PT_CFIndex (*fn_len)(PT_CFStringRef);
    typedef PT_CFIndex (*fn_maxsz)(PT_CFIndex, PT_CFStringEncoding);
    typedef PT_CFIndex (*fn_getbytes)(PT_CFStringRef, PT_CFRange, PT_CFStringEncoding, uint8_t, uint8_t, uint8_t *, PT_CFIndex, PT_CFIndex *);
    typedef void (*fn_release)(const void *);
    static fn_create p_create = NULL;
    static fn_len p_len = NULL;
    static fn_maxsz p_maxsz = NULL;
    static fn_getbytes p_getbytes = NULL;
    static fn_release p_release = NULL;
    if (!p_create) {
        p_create = (fn_create)dlsym(RTLD_DEFAULT, "CFStringCreateWithCString");
        p_len = (fn_len)dlsym(RTLD_DEFAULT, "CFStringGetLength");
        p_maxsz = (fn_maxsz)dlsym(RTLD_DEFAULT, "CFStringGetMaximumSizeForEncoding");
        p_getbytes = (fn_getbytes)dlsym(RTLD_DEFAULT, "CFStringGetBytes");
        p_release = (fn_release)dlsym(RTLD_DEFAULT, "CFRelease");
    }
    if (!p_create || !p_len || !p_maxsz || !p_getbytes || !p_release) return strdup(utf8);
    PT_CFStringRef cf = p_create(NULL, utf8, PT_kCFStringEncodingUTF8);
    if (!cf) return NULL;
    PT_CFIndex len = p_len(cf);
    PT_CFIndex max_sz = p_maxsz(len, PT_kCFStringEncodingDOSJapanese) + 4;
    if (max_sz < 16) max_sz = 16;
    char *out = calloc(1, (size_t)max_sz);
    if (out) {
        PT_CFIndex used = 0;
        PT_CFRange rng = { 0, len };
        p_getbytes(cf, rng, PT_kCFStringEncodingDOSJapanese, '?', 0,
                   (uint8_t *)out, max_sz - 1, &used);
        out[used] = '\0';
    }
    p_release(cf);
    return out;
#elif defined(_WIN32)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *wbuf = malloc((size_t)wlen * sizeof(wchar_t));
    if (!wbuf) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    int slen = WideCharToMultiByte(932, 0, wbuf, -1, NULL, 0, "?", NULL);
    if (slen <= 0) { free(wbuf); return NULL; }
    char *out = calloc(1, (size_t)slen + 4);
    if (out) WideCharToMultiByte(932, 0, wbuf, -1, out, slen, "?", NULL);
    free(wbuf);
    return out;
#else
    return strdup(utf8);
#endif
}

static int is_sjis_lead(uint8_t c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

static int emit_silence_samples(protalker_tts *t, size_t samples)
{
    while (samples > 0 && !t->stopped) {
        size_t chunk = samples > 2048 ? 2048 : samples;
        if (t->cur_cb(t->zero_buf, chunk, t->cur_user)) {
            t->stopped = 1;
            return 1;
        }
        samples -= chunk;
    }
    return t->stopped ? 1 : 0;
}

int protalker_tts_speak(protalker_tts *t, const char *utf8_text, double sapi_rate, double semitones,
                        protalker_audio_cb cb, void *user)
{
    if (!t || !utf8_text || !cb || emu_failed(t->e)) return -1;
    char *sjis = utf8_to_sjis_alloc(utf8_text);
    if (!sjis) return -1;
    if (!*sjis) {
        free(sjis);
        return 0;
    }

    if (!(sapi_rate == sapi_rate)) sapi_rate = 0.0;
    if (!(semitones == semitones)) semitones = 0.0;

    /* Map SAPI rate (-10..18, default 0) to ProTALKER speed (4..16, default 6 = NVDA rate 25 / dwSpeed 120) */
    int speed;
    if (sapi_rate < 0.0) {
        speed = (int)lrint(6.0 + (sapi_rate / 10.0) * (6.0 - 4.0));
    } else {
        speed = (int)lrint(6.0 + (sapi_rate / 18.0) * (16.0 - 6.0));
    }
    if (speed < 4) speed = 4;
    if (speed > 16) speed = 16;

    /* Map semitones (-12..12, default 0) to ProTALKER pitch (50..170, default 97 = NVDA pitch 55.5 midpoint between 55=93 and 56=100) */
    int pitch;
    if (semitones < 0.0) {
        pitch = (int)lrint(97.0 + (semitones / 12.0) * (97.0 - 50.0));
    } else {
        pitch = (int)lrint(97.0 + (semitones / 12.0) * (170.0 - 97.0));
    }
    if (pitch < 50) pitch = 50;
    if (pitch > 170) pitch = 170;

    t->cur_cb = cb;
    t->cur_user = user;
    t->stopped = 0;

    /* Split clauses and insert PauseKuTen=2 / PauseToTen=5 / PauseKaiGyo=5 exactly like Ptengine.dll (0x10004faa) */
    const uint8_t *p = (const uint8_t *)sjis;
    int rc_err = 0;
    while (*p && !t->stopped) {
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;
        const uint8_t *start = p;
        int pause_units = 0;
        while (*p) {
            uint8_t c0 = p[0];
            if (is_sjis_lead(c0) && p[1]) {
                uint8_t c1 = p[1];
                p += 2;
                /* 8142 = 。, 8144 = ． */
                if (c0 == 0x81 && (c1 == 0x42 || c1 == 0x44)) {
                    pause_units = 5; /* PauseToTen default = 5 */
                    break;
                }
                /* 8141 = 、, 8143 = ， */
                if (c0 == 0x81 && (c1 == 0x41 || c1 == 0x43)) {
                    pause_units = 2; /* PauseKuTen default = 2 */
                    break;
                }
            } else {
                p++;
                if (c0 == 0xA1) { /* halfwidth ｡ */
                    pause_units = 5;
                    break;
                }
                if (c0 == 0xA4) { /* halfwidth ､ */
                    pause_units = 2;
                    break;
                }
                if (c0 == '\r' || c0 == '\n') {
                    if (c0 == '\r' && *p == '\n') p++;
                    pause_units = 5; /* PauseKaiGyo default = 5 */
                    break;
                }
            }
        }
        size_t clen = (size_t)(p - start);
        if (clen > 0) {
            char *clause = malloc(clen + 1);
            if (!clause) { rc_err = -1; break; }
            memcpy(clause, start, clen);
            clause[clen] = '\0';

            apply_cntl(t, speed, pitch, 9, 5, 0x004Du);
            uint32_t g_text = emu_strdup(t->e, clause);
            free(clause);
            if (!g_text) { rc_err = -1; break; }

            uint32_t speak_args[6] = { t->handle_val, g_text, 0, 0, 999u, t->cb_thunk };
            uint32_t rc = emu_call(t->e, t->fn_speak, 6, speak_args);
            emu_free(t->e, g_text);
            if (emu_failed(t->e) || (rc != 0 && !t->stopped)) {
                rc_err = -1;
                break;
            }
        }
        if (pause_units > 0 && !t->stopped) {
            size_t sil_samples = (size_t)((22050 * pause_units) / speed);
            emit_silence_samples(t, sil_samples);
        }
    }

    free(sjis);
    t->cur_cb = NULL;
    t->cur_user = NULL;

    if (t->stopped) return 1;
    if (rc_err != 0) return -1;
    return 0;
}
